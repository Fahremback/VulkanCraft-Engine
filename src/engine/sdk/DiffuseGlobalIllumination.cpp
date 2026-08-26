// DiffuseGlobalIllumination.cpp — Agente 1 (task_plan A.5): the HEADLESS
// dynamic diffuse GI (multi-bounce radiosity). Consumes captured material
// cards (A.4) and iterates the diffuse-to-diffuse form factor to produce
// per-card direct + indirect + outgoing radiance, with shadowed skylight and
// emissive sources. Deterministic (Jacobi iteration, no RNG). Self-contained
// (std + glm).

#include "engine/rendering/IDiffuseGlobalIllumination.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace Engine::Rendering {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kEps = 1.0e-6f;

glm::vec3 safe_normalize(const glm::vec3& v, const glm::vec3& fallback) {
    const float lenSq = glm::dot(v, v);
    return lenSq > kEps ? v * glm::inversesqrt(lenSq) : fallback;
}

float area_of(const CapturedCard& c) {
    return 4.0f * c.card.halfExtent.x * c.card.halfExtent.y;
}

class DiffuseGlobalIllumination final : public IDiffuseGlobalIllumination {
public:
    DiffuseGlobalIllumination() = default;

    bool configure(const DiffuseGiConfig& requested, std::string& errorOut) override {
        if (requested.bounces < 1 || requested.bounces > 8) {
            errorOut = "diffuse_gi: bounces must be in [1, 8]";
            return false;
        }
        if (!std::isfinite(requested.maxDistance) || requested.maxDistance <= 0.0f) {
            errorOut = "diffuse_gi: maxDistance must be > 0";
            return false;
        }
        if (!std::isfinite(requested.intensity) ||
            requested.intensity < 0.01f || requested.intensity > 64.0f) {
            errorOut = "diffuse_gi: intensity must be in [0.01, 64]";
            return false;
        }
        if (!std::isfinite(requested.skylight.x) ||
            !std::isfinite(requested.skylight.y) ||
            !std::isfinite(requested.skylight.z)) {
            errorOut = "diffuse_gi: skylight must be finite";
            return false;
        }
        config_ = requested;
        errorOut.clear();
        return true;
    }

    const DiffuseGiConfig& config() const noexcept override { return config_; }

    bool set_cards(const std::vector<CapturedCard>& cards,
                   std::string& errorOut) override {
        for (const CapturedCard& c : cards) {
            if (!std::isfinite(c.card.center.x) || !std::isfinite(c.card.center.y) ||
                !std::isfinite(c.card.center.z)) {
                errorOut = "diffuse_gi: card center must be finite";
                return false;
            }
            if (!std::isfinite(c.card.halfExtent.x) ||
                !std::isfinite(c.card.halfExtent.y) ||
                c.card.halfExtent.x < 0.0f || c.card.halfExtent.y < 0.0f) {
                errorOut = "diffuse_gi: card halfExtent must be finite and >= 0";
                return false;
            }
        }
        cards_ = cards;
        normals_.resize(cards_.size());
        areas_.resize(cards_.size());
        emissive_.resize(cards_.size());
        albedo_.resize(cards_.size());
        direct_.assign(cards_.size(), glm::vec3(0.0f));
        radiosity_.assign(cards_.size(), glm::vec3(0.0f));
        gathered_.assign(cards_.size(), glm::vec3(0.0f));
        outgoing_.assign(cards_.size(), glm::vec3(0.0f));
        for (std::size_t i = 0; i < cards_.size(); ++i) {
            normals_[i] = safe_normalize(cards_[i].card.normal,
                                         glm::vec3(0.0f, 1.0f, 0.0f));
            areas_[i] = std::max(kEps, area_of(cards_[i]));
            emissive_[i] = cards_[i].selfLuminous ? cards_[i].card.emissive
                                                  : glm::vec3(0.0f);
            albedo_[i] = glm::vec3(cards_[i].card.albedo);
        }
        // Reset the solve state (config/cards changed).
        bounceEnergy_.clear();
        solved_ = false;
        errorOut.clear();
        return true;
    }

    bool solve(std::string& errorOut) override {
        if (cards_.empty()) {
            errorOut = "diffuse_gi: no cards bound";
            return false;
        }
        const std::size_t n = cards_.size();
        bounceEnergy_.assign(config_.bounces, 0.0f);

        // Direct: captured irradiance + shadowed skylight.
        for (std::size_t i = 0; i < n; ++i) {
            const glm::vec4 irr = cards_[i].irradiance;
            const float skyVis = cards_[i].selfLuminous ? 0.0f : irr.a;
            direct_[i] = glm::vec3(irr) + config_.skylight * skyVis;
            // Initial radiosity = emissive + albedo * direct (the first "bounce"
            // of direct light into the scene).
            radiosity_[i] = emissive_[i] + albedo_[i] * direct_[i];
        }

        // Form factors (dense, symmetric) computed once (deterministic).
        // f(i,j) = cos_i * cos_j / (pi * d^2) * area_j, clamped to [0,1];
        // culled to maxDistance.
        std::vector<float> formFactors(n * n, 0.0f);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                if (i == j) continue;
                const glm::vec3 delta = cards_[j].card.center - cards_[i].card.center;
                const float distSq = glm::dot(delta, delta);
                const float dist = std::sqrt(distSq);
                if (dist > config_.maxDistance || distSq <= kEps) continue;
                // Cards are TWO-SIDED sheets (a thin surface cache card
                // scatters off both sides), so orientation enters as |cos|.
                const glm::vec3 dir = delta / dist;  // from receiver i toward emitter j
                const float cosI = std::abs(glm::dot(normals_[i], dir));
                const float cosJ = std::abs(glm::dot(normals_[j], -dir));
                if (cosI <= kEps || cosJ <= kEps) continue;
                const float ff =
                    std::min(1.0f, (cosI * cosJ / (kPi * distSq)) * areas_[j]);
                formFactors[i * n + j] = ff;
            }
        }

        // Jacobi multi-bounce: each bounce gathers the PREVIOUS radiosity.
        std::vector<glm::vec3> nextRadiosity(n, glm::vec3(0.0f));
        for (std::uint32_t bounce = 0; bounce < config_.bounces; ++bounce) {
            float energy = 0.0f;
            for (std::size_t i = 0; i < n; ++i) {
                glm::vec3 sum(0.0f);
                for (std::size_t j = 0; j < n; ++j) {
                    if (i == j) continue;
                    sum += formFactors[i * n + j] * radiosity_[j];
                }
                gathered_[i] = sum * config_.intensity;
                // Radiosity updates: emissive + albedo * (direct + gathered).
                nextRadiosity[i] =
                    emissive_[i] + albedo_[i] * (direct_[i] + gathered_[i]);
                energy += glm::dot(gathered_[i], glm::vec3(1.0f));
            }
            bounceEnergy_[bounce] = energy;
            radiosity_.swap(nextRadiosity);
        }

        // Final outgoing = emissive + albedo * (direct + last gathered).
        for (std::size_t i = 0; i < n; ++i) {
            outgoing_[i] = radiosity_[i];
        }

        solved_ = true;
        errorOut.clear();
        return true;
    }

    std::uint32_t card_count() const noexcept override {
        return static_cast<std::uint32_t>(cards_.size());
    }

    bool result(std::uint32_t index, DiffuseGiResult& out) const override {
        if (index >= cards_.size() || !solved_) return false;
        out.direct = direct_[index];
        out.indirect = gathered_[index];
        out.outgoing = outgoing_[index];
        return true;
    }

    float bounce_energy(std::uint32_t bounce) const override {
        if (bounce == 0 || bounce > bounceEnergy_.size()) return 0.0f;
        return bounceEnergy_[bounce - 1];
    }

private:
    DiffuseGiConfig config_{};
    std::vector<CapturedCard> cards_;
    std::vector<glm::vec3> normals_;
    std::vector<float> areas_;
    std::vector<glm::vec3> emissive_;
    std::vector<glm::vec3> albedo_;
    std::vector<glm::vec3> direct_;
    std::vector<glm::vec3> gathered_;
    std::vector<glm::vec3> radiosity_;
    std::vector<glm::vec3> outgoing_;
    std::vector<float> bounceEnergy_;
    bool solved_{ false };
};

}  // namespace

std::unique_ptr<IDiffuseGlobalIllumination> create_diffuse_global_illumination(
    std::string& errorOut) {
    auto gi = std::make_unique<DiffuseGlobalIllumination>();
    DiffuseGiConfig defaults;
    if (!gi->configure(defaults, errorOut)) return nullptr;
    return gi;
}

}  // namespace Engine::Rendering
