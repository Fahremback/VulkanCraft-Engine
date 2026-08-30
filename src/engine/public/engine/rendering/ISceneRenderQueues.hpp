#pragma once

// ISceneRenderQueues — Agente 1 (task_plan B.4), the PUBLIC draw-queue core:
// categorizes submitted scene draw items into the canonical queues (opaca,
// alpha-test, transparente, emissiva, água, foliage, gizmo e UI) and orders
// each queue with the correct key for the real submission pass.
//
// Before this core the only real sub-case of ordering was water drawn
// back-to-front by distance in WorldRenderer::draw_water; every other detail
// queue had no explicit ordering in the submission path. This is that missing
// ordering engine, headless and deterministic:
//   opaque / alphaTest / foliage — front-to-back by camera depth (ascending),
//                                  maximizing early-z;
//   transparent / water / emissive — back-to-front by camera depth
//                                  (descending), correct blending;
//   gizmo / ui — insertion (explicit overlay order).
//
// Self-contained (std); no GPU state, no external headers. The renderer
// pushes items with a *sort key* (a comparable depth/priority) and the
// returned queue is already ordered for the pass. bit-exact for the same
// inputs on every machine.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering {

// The canonical draw queue a submitted item belongs to (task_plan B.4).
enum class DrawQueue : std::uint8_t {
    Opaque = 0,      // solid, early-z, front-to-back
    AlphaTest = 1,   // cutout (discard), front-to-back
    Foliage = 2,     // alpha-test vegetation, front-to-back
    Transparent = 3, // blended, back-to-front
    Emissive = 4,    // emissive/glow, back-to-front (after transparent)
    Water = 5,       // refractive/alpha-by-depth, back-to-front
    Gizmo = 6,       // editor overlays, insertion order
    Ui = 7,          // UI/overlays, insertion order
};

// Returns the stable name of a queue ("opaque", "alphaTest", ...). Used for
// diagnostics / the frame-graph pass names and serialized render-report rows.
const char* draw_queue_name(DrawQueue queue) noexcept;

// A submitted draw item: an opaque payload id + a comparable sort key.
// `depth` is the camera-space depth of the item's representative point and is
// the ordering key for depth-sorted queues; `sequence` breaks ties in
// insertion order (stable). `payload` is the renderer's opaque handle (mesh,
// buffer range, instance id, ...) — the core never dereferences it.
struct SceneDrawItem {
    std::uint64_t payload{ 0 };   // renderer-side draw id (mesh/chunk/buffer)
    float depth{ 0.0f };          // camera-space depth for ordering
    std::uint64_t sequence{ 0 };  // insertion counter (stable tie-break)
};

// Per-queue buffers of the already-ordered draw items. Reading
// sorted(queue) returns items in the exact order the submission pass should
// issue draws.
class ISceneRenderQueues {
public:
    virtual ~ISceneRenderQueues() = default;

    // Appends one item to its queue (does NOT order yet — ordering is applied
    // lazily by sorted()/sorted_all so a full frame's items can be pushed then
    // drained once).
    virtual void push(DrawQueue queue, const SceneDrawItem& item) = 0;

    // Number of pushed items in the queue.
    virtual std::size_t size(DrawQueue queue) const = 0;

    // Orders the queue and returns the draw order for the submission pass.
    // Depth-sorted queues are sorted by their key; gizmo/ui keep insertion
    // order. Returns a stable reference valid until the next push() to the
    // same queue or clear().
    virtual const std::vector<SceneDrawItem>& sorted(DrawQueue queue) = 0;

    // Same as sorted() for every queue, in canonical queue order
    // (opaque, alphaTest, foliage, transparent, emissive, water, gizmo, ui).
    virtual const std::vector<SceneDrawItem>& sorted_all() = 0;

    // Empties every queue (start of a new frame).
    virtual void clear() = 0;
};

// Creates the draw-queue core (the only TU implementing ISceneRenderQueues).
std::unique_ptr<ISceneRenderQueues> create_scene_render_queues();

}  // namespace Engine::Rendering