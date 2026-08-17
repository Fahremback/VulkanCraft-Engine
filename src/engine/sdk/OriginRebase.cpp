// OriginRebase.cpp
//
// The only implementation of engine::world::IOriginRebase (SDK, META §19 /
// FALTANTES §15 "origin rebasing ou coordenadas de alta precisão"). Pure
// composition of the public layers (IWorldManager + IEntityWorld per world) —
// no backend, headless, deterministic.
//
// Model: the ABSOLUTE world frame is the source of truth (double precision).
// The service owns the LOCAL frame origin (double) and stores entity
// positions relative to it (float, small => exact at sub-block resolution).
// A rebase shifts the origin and translates every entity in every world by
// -delta, preserving absolute positions bit-exact for content near the new
// origin (the frame the rebase follows) while the local frame returns to
// float-safe magnitudes.

#include "engine/world/IOriginRebase.hpp"
#include "engine/world/ILocalSpace.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace engine {
namespace world {

namespace {

// Bit-level finiteness check: the project builds with /fp:fast, under which
// std::isfinite(NaN) can be folded to "true" (the compiler assumes NaNs do
// not occur). Reading the exponent bits directly is immune to that.
bool is_finite_double(double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    // Exponent field all ones => Inf/NaN (sign irrelevant).
    return (bits & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL;
}

struct PendingShift {
    std::string worldName;
    engine::entity::EntityId handle;
    engine::entity::Position oldLocal;
    engine::entity::Position newLocal;
};

}  // namespace

class OriginRebase final : public IOriginRebase {
public:
    explicit OriginRebase(IWorldManager& manager) : manager_(manager) {}

    glm::dvec3 origin() const override { return origin_; }

    glm::vec3 to_local(const glm::dvec3& absolute) const override {
        const glm::dvec3 d = absolute - origin_;
        return glm::vec3(static_cast<float>(d.x), static_cast<float>(d.y),
                         static_cast<float>(d.z));
    }

    glm::dvec3 to_absolute_d(const glm::vec3& local) const override {
        return origin_ + glm::dvec3(local);
    }

    RebaseResult rebase(const glm::dvec3& newOrigin,
                        std::string& errorOut) override {
        RebaseResult result;
        const glm::dvec3 delta = newOrigin - origin_;

        // Non-finite first (bit-level; /fp:fast would fold std::isfinite).
        if (!is_finite_double(delta.x) || !is_finite_double(delta.y) ||
            !is_finite_double(delta.z)) {
            errorOut = "origin rebase: non-finite origin delta";
            return result;
        }
        // Zero delta: nothing to do, success.
        if (std::fabs(delta.x) < 1e-12 && std::fabs(delta.y) < 1e-12 &&
            std::fabs(delta.z) < 1e-12) {
            return result;
        }

        // ---- Phase 1: collect (no mutation yet) ----------------------------
        // A shift of the origin by +delta moves every local position by
        // -delta (absolute = origin + local must stay invariant). Computed in
        // double, cast to float — exact because the locals are small.
        std::vector<PendingShift> shifts;
        for (const std::string& name : manager_.world_names()) {
            engine::voxel::IVoxelWorld* world = manager_.world(name);
            if (world == nullptr) {
                errorOut = "origin rebase: world '" + name +
                           "' vanished during collection";
                return result;
            }
            auto* entities = world->entity_world().get();
            if (entities == nullptr) continue;
            entities->for_each_entity([&](engine::entity::EntityId handle) {
                // Entities BOUND to a local space (ILocalSpace) store their
                // position relative to that space's frame, not the world
                // frame — the rebase must NOT translate them (the space
                // transform absorbs the world offset).
                engine::entity::ComponentData spaceRef;
                if (entities->get_component(handle, kSpaceRefComponent,
                                             spaceRef)) {
                    return;
                }
                engine::entity::Position local;
                if (!entities->get_position(handle, local)) return;
                PendingShift shift;
                shift.worldName = name;
                shift.handle = handle;
                shift.oldLocal = local;
                shift.newLocal.x = static_cast<float>(
                    static_cast<double>(local.x) - delta.x);
                shift.newLocal.y = static_cast<float>(
                    static_cast<double>(local.y) - delta.y);
                shift.newLocal.z = static_cast<float>(
                    static_cast<double>(local.z) - delta.z);
                shifts.push_back(shift);
            });
        }

        // ---- Phase 2: apply (transactional) --------------------------------
        for (std::size_t i = 0; i < shifts.size(); ++i) {
            engine::voxel::IVoxelWorld* world =
                manager_.world(shifts[i].worldName);
            if (world == nullptr ||
                !world->entity_world()->set_position(shifts[i].handle,
                                                     shifts[i].newLocal)) {
                // Roll back everything applied so far; the origin is NOT
                // committed — both phases all-or-nothing.
                for (std::size_t j = 0; j < i; ++j) {
                    engine::voxel::IVoxelWorld* w =
                        manager_.world(shifts[j].worldName);
                    if (w != nullptr) {
                        w->entity_world()->set_position(shifts[j].handle,
                                                        shifts[j].oldLocal);
                    }
                }
                errorOut = "origin rebase: failed to shift an entity position";
                return result;
            }
            ++result.translatedEntities;
        }

        origin_ = newOrigin;
        result.rebased = true;
        result.delta = delta;
        return result;
    }

    RebaseResult update(const glm::dvec3& focus, double threshold,
                        bool snapToVoxel, std::string& errorOut) override {
        RebaseResult result;
        if (threshold < 0.0) {
            errorOut = "origin rebase: negative focus threshold";
            return result;
        }
        const glm::dvec3 diff = focus - origin_;
        const double distance =
            std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);
        if (distance <= threshold) return result;  // focus still in range

        glm::dvec3 newOrigin = focus;
        if (snapToVoxel) {
            // Integer voxel snap keeps floor(absolute) == floor(origin) +
            // floor(local) integer-exact.
            newOrigin.x = std::floor(focus.x);
            newOrigin.y = std::floor(focus.y);
            newOrigin.z = std::floor(focus.z);
        }
        return rebase(newOrigin, errorOut);
    }

    engine::entity::EntityId spawn_at(const std::string& worldName,
                                      const std::string& type,
                                      const glm::dvec3& absolute,
                                      std::string& errorOut) override {
        engine::voxel::IVoxelWorld* world = manager_.world(worldName);
        if (world == nullptr) {
            errorOut = "origin rebase: unknown world '" + worldName + "'";
            return {};
        }
        auto* entities = world->entity_world().get();
        if (entities == nullptr) {
            errorOut = "origin rebase: world '" + worldName +
                       "' has no entity layer";
            return {};
        }
        const glm::vec3 local = to_local(absolute);
        engine::entity::Position position{ local.x, local.y, local.z };
        return entities->spawn(type, position, errorOut);
    }

    bool absolute_position(const std::string& worldName,
                           engine::entity::EntityId handle, glm::dvec3& out,
                           std::string& errorOut) const override {
        engine::voxel::IVoxelWorld* world = manager_.world(worldName);
        if (world == nullptr) {
            errorOut = "origin rebase: unknown world '" + worldName + "'";
            return false;
        }
        auto* entities = world->entity_world().get();
        if (entities == nullptr) {
            errorOut = "origin rebase: world '" + worldName +
                       "' has no entity layer";
            return false;
        }
        engine::entity::Position local;
        if (!entities->get_position(handle, local)) {
            errorOut = "origin rebase: dead entity handle";
            return false;
        }
        // A space-bound entity stores its position relative to the space's
        // frame, not the world frame — the rebase service cannot interpret it.
        engine::entity::ComponentData spaceRef;
        if (entities->get_component(handle, kSpaceRefComponent, spaceRef)) {
            errorOut = "origin rebase: entity is bound to a local space; use "
                       "ILocalSpace::entity_world_position";
            return false;
        }
        out = to_absolute_d(glm::vec3(local.x, local.y, local.z));
        return true;
    }

private:
    IWorldManager& manager_;
    glm::dvec3 origin_{ 0.0, 0.0, 0.0 };
};

std::unique_ptr<IOriginRebase> create_origin_rebase(IWorldManager& manager) {
    return std::make_unique<OriginRebase>(manager);
}

}  // namespace world
}  // namespace engine
