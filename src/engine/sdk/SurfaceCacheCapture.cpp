// SurfaceCacheCapture.cpp — Agente 1 (task_plan A.4): the HEADLESS material
// card capture. Binds a surface cache (ILumenScene) to a radiance source and
// captures per-card irradiance + bounced radiance with a per-frame CARD budget,
// nearest-camera-first priority, LOCALIZED invalidation (per chunk), and exact
// VRAM accounting. Self-contained (std + glm), deterministic.

#include "engine/rendering/ISurfaceCacheCapture.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <unordered_map>
#include <vector>

namespace Engine::Rendering {
namespace {

constexpr float kEmitEpsilon = 1.0e-4f;

glm::vec4 default_radiance(const glm::vec3&, const glm::vec3&) {
    return glm::vec4(0.25f, 0.28f, 0.32f, 1.0f);
}

struct ChunkInfo {
    std::uint64_t id{ 0 };
    std::uint64_t revision{ 0 };
    std::vector<LumenSurfaceCard> cards;
    glm::vec3 firstCenter{ 0.0f };
};

struct CapturedChunk {
    std::uint64_t revision{ 0 };
    std::vector<CapturedCard> cards;
    std::uint32_t cursor{ 0 };
    bool pending{ true };
};

class SurfaceCacheCapture final : public ISurfaceCacheCapture {
public:
    SurfaceCacheCapture() = default;

    bool configure(const CaptureConfig& requested, std::string& errorOut) override {
        if (requested.cardsPerFrame < 1) {
            errorOut = "capture: cardsPerFrame must be >= 1";
            return false;
        }
        if (requested.maxCapturedCards < 1) {
            errorOut = "capture: maxCapturedCards must be >= 1";
            return false;
        }
        config_ = requested;
        errorOut.clear();
        return true;
    }

    const CaptureConfig& config() const noexcept override { return config_; }

    void bind_scene(ILumenScene* scene) override {
        scene_ = scene;
        resync();
    }

    void bind_radiance(RadianceSampler sampler) override {
        sampler_ = sampler ? std::move(sampler) : RadianceSampler(&default_radiance);
    }

    bool invalidate_chunk(std::uint64_t chunkId) override {
        resync();
        auto it = captured_.find(chunkId);
        if (it == captured_.end()) return false;
        it->second.cards.clear();
        it->second.cursor = 0;
        it->second.pending = true;
        recompute_counts();
        return true;
    }

    void invalidate_all() override {
        resync();
        for (auto& [id, chunk] : captured_) {
            (void)id;
            chunk.cards.clear();
            chunk.cursor = 0;
            chunk.pending = true;
        }
        recompute_counts();
    }

    std::uint32_t update(const glm::vec3& cameraPosition) override {
        resync();
        const std::uint32_t budget = config_.cardsPerFrame;

        // Order pending chunks nearest-camera first (Lumen "capture near
        // first"), tiebreak by chunk id (deterministic).
        std::vector<std::uint64_t> pendingOrder;
        for (const ChunkInfo& info : orderedChunks_) {
            auto it = captured_.find(info.id);
            if (it == captured_.end()) continue;
            CapturedChunk& c = it->second;
            if (!c.pending || c.cursor >= info.cards.size()) continue;
            pendingOrder.push_back(info.id);
        }
        std::sort(pendingOrder.begin(), pendingOrder.end(),
                  [this, &cameraPosition](std::uint64_t a, std::uint64_t b) {
                      const ChunkInfo* ia = find_chunk(a);
                      const ChunkInfo* ib = find_chunk(b);
                      const float da = ia ? glm::distance(ia->firstCenter, cameraPosition)
                                          : 0.0f;
                      const float db = ib ? glm::distance(ib->firstCenter, cameraPosition)
                                          : 0.0f;
                      if (da != db) return da < db;
                      return a < b;
                  });

        std::uint32_t captured = 0;
        std::uint32_t remaining = budget;
        for (const std::uint64_t id : pendingOrder) {
            if (remaining == 0) break;
            CapturedChunk& c = captured_[id];
            const ChunkInfo* info = find_chunk(id);
            if (info == nullptr) continue;
            while (c.cursor < info->cards.size() && remaining > 0) {
                const LumenSurfaceCard& card = info->cards[c.cursor];
                CapturedCard out;
                out.card = card;
                const bool emissive = glm::dot(card.emissive, card.emissive) >
                                      kEmitEpsilon * kEmitEpsilon;
                if (emissive) {
                    out.selfLuminous = true;
                    out.irradiance = glm::vec4(card.emissive, 1.0f);
                    out.bouncedRadiance = card.emissive;
                } else {
                    const glm::vec4 irr = sampler_(card.center, card.normal);
                    out.irradiance = irr;
                    out.bouncedRadiance =
                        glm::vec3(card.albedo) * glm::vec3(irr);
                }
                out.captureAge = ++age_;
                c.cards.push_back(std::move(out));
                ++c.cursor;
                ++captured;
                --remaining;
            }
            if (c.cursor >= info->cards.size()) c.pending = false;
        }

        recompute_counts();
        return captured;
    }

    std::uint32_t captured_count() const noexcept override { return capturedCount_; }
    std::uint32_t pending_count() const noexcept override { return pendingCount_; }

    bool captured(std::uint32_t index, CapturedCard& out) const override {
        std::uint32_t seen = 0;
        for (const ChunkInfo& info : orderedChunks_) {
            auto it = captured_.find(info.id);
            if (it == captured_.end()) continue;
            const std::vector<CapturedCard>& cards = it->second.cards;
            if (index - seen < cards.size()) {
                out = cards[index - seen];
                return true;
            }
            seen += static_cast<std::uint32_t>(cards.size());
        }
        return false;
    }

    std::uint64_t vram_bytes() const noexcept override {
        return static_cast<std::uint64_t>(capturedCount_) * kCapturedCardVramBytes;
    }

    std::uint64_t capture_age() const noexcept override { return age_; }

private:
    const ChunkInfo* find_chunk(std::uint64_t id) const {
        for (const ChunkInfo& info : orderedChunks_) {
            if (info.id == id) return &info;
        }
        return nullptr;
    }

    // Rebuilds the scene's chunk order from the bound scene and reconciles the
    // captured state (drop gone chunks, re-capture revision-changed chunks).
    void resync() {
        orderedChunks_.clear();
        if (scene_ == nullptr) {
            captured_.clear();
            recompute_counts();
            return;
        }

        // Pass 1: discover unique chunks in scene order (id -> stable index).
        std::unordered_map<std::uint64_t, std::size_t> byId;
        for (std::uint32_t i = 0; i < scene_->card_count(); ++i) {
            LumenSurfaceCard card{};
            if (!scene_->card(i, card)) continue;
            auto it = byId.find(card.chunkId);
            if (it == byId.end()) {
                ChunkInfo info;
                info.id = card.chunkId;
                info.revision = card.revision;
                info.firstCenter = card.center;
                byId[card.chunkId] = orderedChunks_.size();
                orderedChunks_.push_back(std::move(info));
            }
        }
        // Pass 2: fill the cards of each chunk (index-based, no dangling refs).
        for (std::uint32_t i = 0; i < scene_->card_count(); ++i) {
            LumenSurfaceCard card{};
            if (!scene_->card(i, card)) continue;
            orderedChunks_[byId[card.chunkId]].cards.push_back(card);
        }

        // Drop captured chunks no longer present.
        for (auto it = captured_.begin(); it != captured_.end();) {
            if (byId.find(it->first) == byId.end()) {
                it = captured_.erase(it);
            } else {
                ++it;
            }
        }

        // (Re)open chunks whose revision changed or that are new.
        for (const ChunkInfo& info : orderedChunks_) {
            auto it = captured_.find(info.id);
            if (it == captured_.end()) {
                CapturedChunk c;
                c.revision = info.revision;
                c.pending = true;
                captured_[info.id] = std::move(c);
            } else if (it->second.revision != info.revision) {
                CapturedChunk c;
                c.revision = info.revision;
                c.pending = true;
                captured_[info.id] = std::move(c);
            }
        }

        recompute_counts();
    }

    void recompute_counts() {
        capturedCount_ = 0;
        pendingCount_ = 0;
        for (const ChunkInfo& info : orderedChunks_) {
            auto it = captured_.find(info.id);
            if (it == captured_.end()) {
                pendingCount_ += static_cast<std::uint32_t>(info.cards.size());
                continue;
            }
            capturedCount_ += static_cast<std::uint32_t>(it->second.cards.size());
            if (it->second.cursor < info.cards.size()) {
                pendingCount_ += static_cast<std::uint32_t>(info.cards.size() -
                                                            it->second.cursor);
            }
        }
    }

    CaptureConfig config_{};
    ILumenScene* scene_{ nullptr };
    RadianceSampler sampler_{ &default_radiance };
    std::vector<ChunkInfo> orderedChunks_;
    std::unordered_map<std::uint64_t, CapturedChunk> captured_;
    std::uint64_t age_{ 0 };
    std::uint32_t capturedCount_{ 0 };
    std::uint32_t pendingCount_{ 0 };
};

}  // namespace

std::unique_ptr<ISurfaceCacheCapture> create_surface_cache_capture(
    std::string& errorOut) {
    auto capture = std::make_unique<SurfaceCacheCapture>();
    CaptureConfig defaults;
    if (!capture->configure(defaults, errorOut)) return nullptr;
    return capture;
}

}  // namespace Engine::Rendering
