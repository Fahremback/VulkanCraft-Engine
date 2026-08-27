#pragma once

// ISceneLayers (agente 2 — item `openusd`): PUBLIC scene-INTERCHANGE
// contract — layered scene composition in the spirit of USD's core model:
// a scene is a STACK of layers; each layer carries entity records (id ->
// typed properties); composed values resolve by LAYER PRECEDENCE (strongest
// layer wins; later layers in the stack override earlier ones), with
// per-entity "over" records that only override authored properties. This is
// the function USD serves for DCC interchange (non-destructive layering,
// overrides, resolution) — implemented here headless and deterministic, so
// the offline cooker can round-trip authored scenes without a USD runtime.
//
// Design (deterministic, pure):
//   - Layer: id + ordered map of entity id -> property map (name -> value).
//     Values are floats or vec3 (colors/positions); all-or-nothing parsing.
//   - Composition: given a stack [strongest ... weakest], each entity's
//     final property set = strongest value for each property name across
//     layers; entities present only in weaker layers are still emitted.
//   - "Over" records: a property marked over=true in a stronger layer only
//     replaces the property if it exists in a weaker layer (USD over vs
//     define semantics), so stronger layers can add new properties without
//     clobbering, and weaker-authored properties survive.
//   - Deterministic: same layers -> identical composed scene, bit-exact.
//
// Self-contained (std only). The SDK adapter (src/engine/sdk/SceneLayers.cpp)
// is the ONLY TU with behavior.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace assets {

// A single typed property value (float or vec3).
struct LayerValue {
    bool isVec3{ false };
    float scalar{ 0.0f };
    float vec3[3]{ 0.0f, 0.0f, 0.0f };
};

// One entity record: id -> ordered property name -> value (+ over flag).
struct LayerEntity {
    std::string id;
    std::vector<std::string> propNames;
    std::vector<LayerValue> propValues;
    std::vector<bool> propOver;
};

// One layer in the stack.
struct SceneLayer {
    std::string id;
    std::vector<LayerEntity> entities;
};

// Composed result: entity id -> resolved properties (strongest wins).
struct ComposedEntity {
    std::string id;
    std::vector<std::string> propNames;
    std::vector<LayerValue> propValues;
};

class ISceneLayers {
public:
    virtual ~ISceneLayers() = default;

    // Composes a stack of layers into a scene. `layers` is ordered
    // [strongest ... weakest] (index 0 = strongest). All-or-nothing: an
    // empty stack or a layer with no entities is refused. On success the
    // composed scene is returned in `out`.
    virtual bool compose(const std::vector<SceneLayer>& layers,
                         std::vector<ComposedEntity>& out,
                         std::string& errorOut) = 0;
};

// Factory (implemented by the SDK adapter — the only TU with behavior).
std::shared_ptr<ISceneLayers> create_scene_layers();

}  // namespace assets
}  // namespace engine
