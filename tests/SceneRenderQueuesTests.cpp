// SceneRenderQueuesTests — Agente 1 task_plan B.4 (draw-queue core): the
// canonical ordering engine must sort opaque/alphaTest/foliage front-to-back,
// transparent/emissive/water back-to-front, and keep gizmo/ui in insertion
// order — with stable tie-breaks and lazy one-time sorting.
#include "engine/rendering/ISceneRenderQueues.hpp"
#include <cassert>
#include <cstdio>

using Engine::Rendering::DrawQueue;
using Engine::Rendering::SceneDrawItem;
using Engine::Rendering::create_scene_render_queues;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::abort();
    }
}

static std::vector<std::uint64_t> payloads(const std::vector<SceneDrawItem>& v) {
    std::vector<std::uint64_t> out;
    for (const auto& i : v) out.push_back(i.payload);
    return out;
}

int main() {
    auto q = create_scene_render_queues();
    check(q != nullptr, "create_scene_render_queues");

    // Opaque: front-to-back (deepest first in submission, nearest first in draw).
    q->push(DrawQueue::Opaque, SceneDrawItem{ 1, 90.0f, 0 });
    q->push(DrawQueue::Opaque, SceneDrawItem{ 2, 5.0f, 1 });
    q->push(DrawQueue::Opaque, SceneDrawItem{ 3, 40.0f, 2 });
    auto opaque = q->sorted(DrawQueue::Opaque);
    check(payloads(opaque) == std::vector<std::uint64_t>({ 2, 3, 1 }),
          "opaque front-to-back depth order");

    // Opaque tie-break: stable by sequence when depths equal.
    q->clear();
    q->push(DrawQueue::Opaque, SceneDrawItem{ 1, 7.0f, 0 });
    q->push(DrawQueue::Opaque, SceneDrawItem{ 2, 7.0f, 1 });
    q->push(DrawQueue::Opaque, SceneDrawItem{ 3, 2.0f, 2 });
    auto opaqueStable = q->sorted(DrawQueue::Opaque);
    check(payloads(opaqueStable) == std::vector<std::uint64_t>({ 3, 1, 2 }),
          "opaque stable tie-break by insertion sequence");

    // Water: back-to-front (nearest first in submission -> farthest first draw).
    q->clear();
    q->push(DrawQueue::Water, SceneDrawItem{ 1, 10.0f, 0 });
    q->push(DrawQueue::Water, SceneDrawItem{ 2, 44.0f, 1 });
    q->push(DrawQueue::Water, SceneDrawItem{ 3, 12.0f, 2 });
    auto water = q->sorted(DrawQueue::Water);
    check(payloads(water) == std::vector<std::uint64_t>({ 2, 3, 1 }),
          "water back-to-front depth order");

    // Transparent: back-to-front.
    q->clear();
    q->push(DrawQueue::Transparent, SceneDrawItem{ 1, 1.0f, 0 });
    q->push(DrawQueue::Transparent, SceneDrawItem{ 2, 3.0f, 1 });
    auto transparent = q->sorted(DrawQueue::Transparent);
    check(payloads(transparent) == std::vector<std::uint64_t>({ 2, 1 }),
          "transparent back-to-front depth order");

    // Gizmo / UI: insertion order (never re-sorted by depth).
    q->clear();
    q->push(DrawQueue::Gizmo, SceneDrawItem{ 1, 99.0f, 0 });
    q->push(DrawQueue::Gizmo, SceneDrawItem{ 2, 1.0f, 1 });
    auto gizmo = q->sorted(DrawQueue::Gizmo);
    check(payloads(gizmo) == std::vector<std::uint64_t>({ 1, 2 }),
          "gizmo keeps insertion order");
    q->push(DrawQueue::Ui, SceneDrawItem{ 8, 50.0f, 0 });
    q->push(DrawQueue::Ui, SceneDrawItem{ 9, 5.0f, 1 });
    auto ui = q->sorted(DrawQueue::Ui);
    check(payloads(ui) == std::vector<std::uint64_t>({ 8, 9 }),
          "ui keeps insertion order");

    // sorted_all: canonical queue order with each queue internally sorted.
    q->clear();
    q->push(DrawQueue::Water, SceneDrawItem{ 50, 5.0f, 0 });
    q->push(DrawQueue::Opaque, SceneDrawItem{ 10, 8.0f, 1 });
    q->push(DrawQueue::Opaque, SceneDrawItem{ 11, 2.0f, 2 });
    const auto all = q->sorted_all();
    // canonical order: opaque,... -> water. opaque sorted {11,10}, then water {50}.
    check(all.size() == 3, "sorted_all includes every queue");
    check(all[0].payload == 11 && all[1].payload == 10 && all[2].payload == 50,
          "sorted_all canonical queue order + internal sorts");

    // size + clear.
    check(q->size(DrawQueue::Opaque) == 2, "size opaque");
    q->clear();
    check(q->size(DrawQueue::Opaque) == 0 && q->size(DrawQueue::Water) == 0,
          "clear empties all queues");

    // queue names (used by the frame-graph pass rows).
    check(Engine::Rendering::draw_queue_name(DrawQueue::Opaque) == std::string("opaque"),
          "queue name opaque");
    check(Engine::Rendering::draw_queue_name(DrawQueue::Water) == std::string("water"),
          "queue name water");

    std::printf("SceneRenderQueuesTests: all checks passed\n");
    return 0;
}