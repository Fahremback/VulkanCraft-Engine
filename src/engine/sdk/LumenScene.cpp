// LumenScene.cpp — Agente 1 (task_plan A.3): the HEADLESS Lumen-style surface
// cache. It consumes oriented surface patches (fed by the renderer's
// mesh->surface pass) and maintains the deterministic surface CARD cache the
// GI/reflection passes trace against, with:
//   * INCREMENTAL dirty-chunk updates (`replace_chunk` monotonic + `remove_chunk`);
//   * a shared carding rule (`build_cards`) that merges coplanar/adjacent
//     surfaces into one card whose tangent-plane extent grows to the union;
//   * a global + per-chunk card BUDGET with deterministic LRU eviction;
//   * hierarchical DISTANT representation via per-card distance cascades.
// Self-contained (std + glm), no Vulkan, no voxel types.

#include "engine/rendering/ILumenScene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <unordered_map>

namespace Engine::Rendering {
namespace {

constexpr float kEps = 1.0e-6f;
constexpr float kNormalMinLengthSquared = 1.0e-8f;
constexpr float kAlbedoMergeEpsilon = 0.06f;
constexpr std::uint32_t kMaxCascades = 8;

bool finite3(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Deterministic tangent frame for a unit normal (stable branchless up vector).
void tangent_frame(const glm::vec3& n, glm::vec3& u, glm::vec3& v) {
    const glm::vec3 up =
        std::abs(n.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    u = glm::normalize(glm::cross(up, n));
    v = glm::cross(n, u);
}

glm::vec3 normalize_or(const glm::vec3& v, const glm::vec3& fallback) {
    const float lenSq = glm::dot(v, v);
    return lenSq > kNormalMinLengthSquared ? v * glm::inversesqrt(lenSq) : fallback;
}

float area(const LumenSurfaceCard& c) { return c.halfExtent.x * c.halfExtent.y; }

}  // namespace

std::vector<LumenSurfaceCard> build_cards(
    std::uint64_t chunkId, std::uint64_t revision,
    const std::vector<LumenSurface>& surfaces, const LumenSceneConfig& config) {
    // Stable order: group near-parallel normals together (quantized), then by
    // center, so coplanar merge candidates are adjacent and the result is
    // reproducible.
    std::vector<LumenSurface> ordered = surfaces;
    std::sort(ordered.begin(), ordered.end(),
              [](const LumenSurface& a, const LumenSurface& b) {
                  auto bucket = [](const glm::vec3& n) {
                      return std::array<int, 3>{
                          static_cast<int>(std::lround(n.x * 16.0f)),
                          static_cast<int>(std::lround(n.y * 16.0f)),
                          static_cast<int>(std::lround(n.z * 16.0f)) };
                  };
                  const std::array<int, 3> ka = bucket(a.normal);
                  const std::array<int, 3> kb = bucket(b.normal);
                  if (ka != kb) return ka < kb;
                  if (a.center.x != b.center.x) return a.center.x < b.center.x;
                  if (a.center.y != b.center.y) return a.center.y < b.center.y;
                  return a.center.z < b.center.z;
              });

    std::vector<LumenSurfaceCard> cards;
    for (const LumenSurface& s : ordered) {
        const glm::vec3 normal = normalize_or(s.normal, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::vec2 halfExtent{ std::max(0.0f, s.halfExtent.x),
                                    std::max(0.0f, s.halfExtent.y) };

        // Find the first compatible card to merge into (deterministic order).
        LumenSurfaceCard* target = nullptr;
        glm::vec3 tu{}, tv{};
        tangent_frame(normal, tu, tv);
        for (LumenSurfaceCard& c : cards) {
            if (glm::dot(c.normal, normal) < config.coplanarDot) continue;
            // Tangent-plane distance (coplanar adjacency, not altitude).
            const glm::vec3 delta = s.center - c.center;
            const glm::vec3 alongPlane = delta - glm::dot(delta, c.normal) * c.normal;
            if (glm::length(alongPlane) > config.mergeDistance) continue;
            if (glm::length(glm::vec3(c.albedo) - glm::vec3(s.albedo)) >
                kAlbedoMergeEpsilon)
                continue;
            target = &c;
            break;
        }

        if (target == nullptr) {
            LumenSurfaceCard card{};
            card.center = s.center;
            card.normal = normal;
            card.halfExtent = halfExtent;
            card.albedo = s.albedo;
            card.emissive = s.emissive;
            card.chunkId = chunkId;
            card.revision = revision;
            cards.push_back(card);
            continue;
        }

        // Merge: grow the card's tangent-plane extent to the UNION of its own
        // corners and the new surface's corners (axis-aligned in the card's
        // tangent frame — coplanar surfaces share the frame within tolerance).
        glm::vec3 u{}, v{};
        tangent_frame(target->normal, u, v);
        const glm::vec3 origin = target->center;
        float uMin = std::numeric_limits<float>::max();
        float uMax = -std::numeric_limits<float>::max();
        float vMin = std::numeric_limits<float>::max();
        float vMax = -std::numeric_limits<float>::max();
        const auto expand = [&](const glm::vec3& c, const glm::vec2& he) {
            const glm::vec3 rel = c - origin;
            const float cu = glm::dot(rel, u);
            const float cv = glm::dot(rel, v);
            for (float su : { -he.x, he.x }) {
                for (float sv : { -he.y, he.y }) {
                    const float pu = cu + su;
                    const float pv = cv + sv;
                    uMin = std::min(uMin, pu);
                    uMax = std::max(uMax, pu);
                    vMin = std::min(vMin, pv);
                    vMax = std::max(vMax, pv);
                }
            }
        };
        expand(target->center, target->halfExtent);
        expand(s.center, halfExtent);

        const float newHalfU = (uMax - uMin) * 0.5f;
        const float newHalfV = (vMax - vMin) * 0.5f;
        const glm::vec3 newCenter =
            origin + ((uMin + uMax) * 0.5f) * u + ((vMin + vMax) * 0.5f) * v;

        // Area-weighted albedo/emissive average (deterministic).
        const float oldArea = std::max(kEps, area(*target));
        const float newArea = std::max(kEps, halfExtent.x * halfExtent.y);
        const float total = oldArea + newArea;
        target->albedo = (target->albedo * oldArea + s.albedo * newArea) / total;
        target->emissive =
            (target->emissive * oldArea + s.emissive * newArea) / total;
        target->center = newCenter;
        target->halfExtent = glm::vec2(newHalfU, newHalfV);
    }

    // Normalize the (possibly averaged) albedo alpha to keep alpha == 1 for
    // opaque cards unless the inputs varied it (preserve determinism).
    for (LumenSurfaceCard& c : cards) {
        c.albedo.a = glm::clamp(c.albedo.a, 0.0f, 1.0f);
    }
    return cards;
}

namespace {

struct ChunkState {
    std::uint64_t revision{ 0 };
    std::vector<LumenSurfaceCard> cards;
};

class LumenScene final : public ILumenScene {
public:
    LumenScene() = default;

    bool configure(const LumenSceneConfig& requested, std::string& errorOut) override {
        if (requested.maxCards < 1 || requested.maxCards > 65536) {
            errorOut = "lumen_scene: maxCards must be in [1, 65536]";
            return false;
        }
        if (requested.maxCardsPerChunk < 1 ||
            requested.maxCardsPerChunk > requested.maxCards) {
            errorOut = "lumen_scene: maxCardsPerChunk must be in [1, maxCards]";
            return false;
        }
        if (requested.coplanarDot < 0.5f || requested.coplanarDot > 1.0f) {
            errorOut = "lumen_scene: coplanarDot must be in [0.5, 1]";
            return false;
        }
        if (!std::isfinite(requested.mergeDistance) || requested.mergeDistance < 0.0f) {
            errorOut = "lumen_scene: mergeDistance must be >= 0";
            return false;
        }
        if (requested.cascadeCount < 1 || requested.cascadeCount > kMaxCascades) {
            errorOut = "lumen_scene: cascadeCount must be in [1, 8]";
            return false;
        }
        if (!std::isfinite(requested.cascadeDistance) || requested.cascadeDistance <= 0.0f) {
            errorOut = "lumen_scene: cascadeDistance must be > 0";
            return false;
        }
        config_ = requested;
        errorOut.clear();
        return true;
    }

    const LumenSceneConfig& config() const noexcept override { return config_; }

    bool replace_chunk(std::uint64_t chunkId, std::uint64_t revision,
                       const std::vector<LumenSurface>& surfaces,
                       std::string& errorOut) override {
        // Validate the whole input first (all-or-nothing).
        for (const LumenSurface& s : surfaces) {
            if (!finite3(s.center)) {
                errorOut = "lumen_scene: surface center must be finite";
                return false;
            }
            if (!finite3(s.normal) ||
                glm::dot(s.normal, s.normal) <= kNormalMinLengthSquared) {
                errorOut = "lumen_scene: surface normal must be finite and non-zero";
                return false;
            }
            if (!std::isfinite(s.halfExtent.x) || !std::isfinite(s.halfExtent.y) ||
                s.halfExtent.x < 0.0f || s.halfExtent.y < 0.0f) {
                errorOut = "lumen_scene: surface halfExtent must be finite and >= 0";
                return false;
            }
            if (!finite3(glm::vec3(s.albedo)) || !std::isfinite(s.albedo.a)) {
                errorOut = "lumen_scene: surface albedo must be finite";
                return false;
            }
            if (!finite3(s.emissive)) {
                errorOut = "lumen_scene: surface emissive must be finite";
                return false;
            }
        }

        auto existing = chunks_.find(chunkId);
        if (existing != chunks_.end() && revision <= existing->second.revision) {
            errorOut = "lumen_scene: stale chunk revision (must be > stored revision)";
            return false;
        }

        std::vector<LumenSurfaceCard> merged =
            build_cards(chunkId, revision, surfaces, config_);
        if (merged.size() > config_.maxCardsPerChunk) {
            // Per-chunk budget (deterministic): keep the first cards in the
            // stable emit order, drop the lowest-priority tail.
            merged.resize(config_.maxCardsPerChunk);
        }

        // Remove the previous incarnation's cards from the running total.
        if (existing != chunks_.end()) {
            totalCards_ -= static_cast<std::uint32_t>(existing->second.cards.size());
            existing->second.cards = std::move(merged);
            existing->second.revision = revision;
        } else {
            ChunkState state;
            state.revision = revision;
            state.cards = std::move(merged);
            chunks_[chunkId] = std::move(state);
        }
        totalCards_ += static_cast<std::uint32_t>(chunks_[chunkId].cards.size());

        // Move to the most-recently-used front.
        order_.erase(std::remove(order_.begin(), order_.end(), chunkId),
                     order_.end());
        order_.push_front(chunkId);

        // Global budget: evict the least-recently-used chunks (back) first.
        while (totalCards_ > config_.maxCards && order_.size() > 1) {
            const std::uint64_t victim = order_.back();
            order_.pop_back();
            auto it = chunks_.find(victim);
            if (it != chunks_.end()) {
                totalCards_ -= static_cast<std::uint32_t>(it->second.cards.size());
                chunks_.erase(it);
            }
        }

        errorOut.clear();
        return true;
    }

    bool remove_chunk(std::uint64_t chunkId) override {
        auto it = chunks_.find(chunkId);
        if (it == chunks_.end()) return false;
        totalCards_ -= static_cast<std::uint32_t>(it->second.cards.size());
        chunks_.erase(it);
        order_.erase(std::remove(order_.begin(), order_.end(), chunkId),
                     order_.end());
        return true;
    }

    std::uint32_t card_count() const noexcept override { return totalCards_; }

    bool card(std::uint32_t index, LumenSurfaceCard& out) const override {
        std::uint32_t seen = 0;
        for (const std::uint64_t id : order_) {
            auto it = chunks_.find(id);
            if (it == chunks_.end()) continue;
            const std::vector<LumenSurfaceCard>& cards = it->second.cards;
            if (index - seen < cards.size()) {
                out = cards[index - seen];
                return true;
            }
            seen += static_cast<std::uint32_t>(cards.size());
        }
        return false;
    }

    void update(const glm::vec3& cameraPosition) override {
        cardsPerCascade_.fill(0);
        for (auto& [id, state] : chunks_) {
            (void)id;
            for (LumenSurfaceCard& c : state.cards) {
                const float dist = glm::distance(c.center, cameraPosition);
                std::uint8_t band = 0;
                float bandDistance = config_.cascadeDistance;
                for (std::uint32_t b = 0; b + 1 < config_.cascadeCount; ++b) {
                    if (dist < bandDistance) break;
                    ++band;
                    bandDistance *= 2.0f;
                }
                c.cascade = band;
                if (band < kMaxCascades) ++cardsPerCascade_[band];
            }
        }
    }

    bool has_chunk(std::uint64_t chunkId) const noexcept override {
        return chunks_.find(chunkId) != chunks_.end();
    }
    std::uint64_t chunk_revision(std::uint64_t chunkId) const noexcept override {
        auto it = chunks_.find(chunkId);
        return it == chunks_.end() ? 0 : it->second.revision;
    }
    std::uint32_t chunk_card_count(std::uint64_t chunkId) const noexcept override {
        auto it = chunks_.find(chunkId);
        return it == chunks_.end() ? 0
                                   : static_cast<std::uint32_t>(it->second.cards.size());
    }
    std::uint32_t cards_in_cascade(std::uint8_t cascade) const override {
        return cascade < kMaxCascades ? cardsPerCascade_[cascade] : 0;
    }

private:
    LumenSceneConfig config_{};
    std::unordered_map<std::uint64_t, ChunkState> chunks_;
    std::deque<std::uint64_t> order_;  // most-recently-used at front
    std::uint32_t totalCards_{ 0 };
    std::array<std::uint32_t, kMaxCascades> cardsPerCascade_{};
};

}  // namespace

std::unique_ptr<ILumenScene> create_lumen_scene(std::string& errorOut) {
    auto scene = std::make_unique<LumenScene>();
    LumenSceneConfig defaults;
    if (!scene->configure(defaults, errorOut)) return nullptr;
    return scene;
}

}  // namespace Engine::Rendering
