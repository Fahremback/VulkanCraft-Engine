// SceneRenderQueues.cpp — the only TU implementing ISceneRenderQueues
// (Agente 1 task_plan B.4): canonical per-queue ordering engine for the real
// submission pass. Headless and deterministic (std only). Before this file
// the only ordering real sub-case was water back-to-front in WorldRenderer;
// this core gives every canonical queue its correct explicit order.
//
// Ordering rules (task_plan B.4):
//   opaque / alphaTest / foliage — front-to-back by camera depth (ascending)
//                                   to maximize early-z rejections;
//   transparent / emissive / water — back-to-front by camera depth
//                                   (descending) for correct blending;
//   gizmo / ui — insertion order (explicit overlay order, never re-sorted).
//
// Ties are broken by `sequence` (insertion counter), so the core is STABLE:
// two items at the same depth keep their submitted order within a queue.
// Sorting is deterministic (std::stable_sort on (depth, sequence)).

#include "engine/rendering/ISceneRenderQueues.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>

namespace Engine::Rendering {

namespace {

bool depth_sorted_front_to_back(DrawQueue queue) noexcept {
    return queue == DrawQueue::Opaque || queue == DrawQueue::AlphaTest ||
           queue == DrawQueue::Foliage;
}

// Canonical queue iteration order (opaque, alphaTest, foliage, transparent,
// emissive, water, gizmo, ui) — the order the submission pass drains sorted_all.
constexpr std::array<DrawQueue, 8> kQueueOrder = {
    DrawQueue::Opaque, DrawQueue::AlphaTest, DrawQueue::Foliage,
    DrawQueue::Transparent, DrawQueue::Emissive, DrawQueue::Water,
    DrawQueue::Gizmo, DrawQueue::Ui,
};

class SceneRenderQueues final : public ISceneRenderQueues {
public:
    void push(DrawQueue queue, const SceneDrawItem& item) override {
        const std::size_t index = static_cast<std::size_t>(queue);
        if (index >= queues_.size()) return;  // invalid queue: refused, no state change
        queues_[index].push_back(item);
    }

    std::size_t size(DrawQueue queue) const override {
        const std::size_t index = static_cast<std::size_t>(queue);
        if (index >= queues_.size()) return 0;
        return queues_[index].size();
    }

    const std::vector<SceneDrawItem>& sorted(DrawQueue queue) override {
        const std::size_t index = static_cast<std::size_t>(queue);
        if (index >= queues_.size()) return empty_;
        if (sorted_[index] != nullptr) return *sorted_[index];
        auto out = std::make_unique<std::vector<SceneDrawItem>>();
        out->reserve(queues_[index].size());
        for (const SceneDrawItem& item : queues_[index]) out->push_back(item);
        const bool frontToBack = depth_sorted_front_to_back(queue);
        std::stable_sort(out->begin(), out->end(),
                         [frontToBack](const SceneDrawItem& a, const SceneDrawItem& b) {
                             if (a.depth != b.depth) {
                                 return frontToBack ? (a.depth < b.depth)
                                                    : (a.depth > b.depth);
                             }
                             return a.sequence < b.sequence;
                         });
        sorted_[index] = std::move(out);
        return *sorted_[index];
    }

    const std::vector<SceneDrawItem>& sorted_all() override {
        ordered_.clear();
        for (DrawQueue queue : kQueueOrder) {
            const auto& rows = sorted(queue);
            ordered_.insert(ordered_.end(), rows.begin(), rows.end());
        }
        return ordered_;
    }

    void clear() override {
        for (auto& q : queues_) q.clear();
        for (auto& s : sorted_) s.reset();
        ordered_.clear();
    }

private:
    std::array<std::vector<SceneDrawItem>, 8> queues_;
    std::array<std::unique_ptr<std::vector<SceneDrawItem>>, 8> sorted_;
    std::vector<SceneDrawItem> ordered_;
    std::vector<SceneDrawItem> empty_;
};

}  // namespace

const char* draw_queue_name(DrawQueue queue) noexcept {
    switch (queue) {
        case DrawQueue::Opaque: return "opaque";
        case DrawQueue::AlphaTest: return "alphaTest";
        case DrawQueue::Foliage: return "foliage";
        case DrawQueue::Transparent: return "transparent";
        case DrawQueue::Emissive: return "emissive";
        case DrawQueue::Water: return "water";
        case DrawQueue::Gizmo: return "gizmo";
        case DrawQueue::Ui: return "ui";
    }
    return "unknown";
}

std::unique_ptr<ISceneRenderQueues> create_scene_render_queues() {
    return std::make_unique<SceneRenderQueues>();
}

}  // namespace Engine::Rendering