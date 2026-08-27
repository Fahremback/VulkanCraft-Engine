// SceneLayers.cpp — SDK adapter for engine/assets/ISceneLayers.hpp
// (agente 2, item `openusd`). The ONLY TU with behavior. Implements the
// layered scene-composition core (the function USD serves for DCC
// interchange) headless and deterministic:
//   - stack [strongest ... weakest]: strongest layer wins per property;
//   - USD-style "over": a property flagged over=true in a stronger layer
//     only replaces a property ALREADY authored in a weaker layer (does not
//     introduce new properties);
//   - entities present in any layer are emitted; stronger layers can
//     redefine an entity's properties.
//
// The engine has no vendored USD; this adapter provides the composition
// core the offline cooker needs (non-destructive layered interchange).

#include "engine/assets/ISceneLayers.hpp"

#include <string>
#include <utility>
#include <vector>

namespace engine {
namespace assets {

namespace {

// Finds the property index by name in an entity, or -1.
int find_prop(const LayerEntity& e, const std::string& name) {
    for (std::size_t i = 0; i < e.propNames.size(); ++i) {
        if (e.propNames[i] == name) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

class SceneLayers final : public ISceneLayers {
public:
    bool compose(const std::vector<SceneLayer>& layers,
                 std::vector<ComposedEntity>& out,
                 std::string& errorOut) override {
        out.clear();
        if (layers.empty()) {
            errorOut = "scene layers: empty layer stack";
            return false;
        }
        // Collect all entity ids across layers (in first-seen order).
        std::vector<std::string> entityIds;
        for (const SceneLayer& layer : layers) {
            if (layer.entities.empty()) {
                errorOut = "scene layers: layer '" + layer.id + "' has no entities";
                return false;
            }
            for (const LayerEntity& e : layer.entities) {
                if (e.id.empty()) {
                    errorOut = "scene layers: empty entity id";
                    return false;
                }
                if (e.propNames.size() != e.propValues.size() ||
                    e.propNames.size() != e.propOver.size()) {
                    errorOut = "scene layers: entity '" + e.id +
                               "' has mismatched property arrays";
                    return false;
                }
                bool seen = false;
                for (const std::string& id : entityIds) {
                    if (id == e.id) { seen = true; break; }
                }
                if (!seen) entityIds.push_back(e.id);
            }
        }
        if (entityIds.empty()) {
            errorOut = "scene layers: no entities across the stack";
            return false;
        }

        // Compose per entity: strongest layer wins; over records only
        // replace properties authored in a weaker layer.
        for (const std::string& id : entityIds) {
            ComposedEntity composed;
            composed.id = id;
            // Phase 1: collect the DEFINING (non-over) properties weakest
            // first so stronger over records can see what exists below.
            // We walk layers strongest -> weakest, but for over semantics we
            // need to know "authored in a weaker layer". Simplest correct
            // approach: first pass weakest -> strongest to collect authored
            // names, then strongest -> weakest to resolve values.
            std::vector<std::string> authoredNames;
            for (std::size_t li = layers.size(); li > 0; --li) {
                const SceneLayer& layer = layers[li - 1];
                for (const LayerEntity& e : layer.entities) {
                    if (e.id != id) continue;
                    for (std::size_t pi = 0; pi < e.propNames.size(); ++pi) {
                        if (e.propOver[pi]) continue;  // over is not a define
                        bool has = false;
                        for (const std::string& n : authoredNames) {
                            if (n == e.propNames[pi]) { has = true; break; }
                        }
                        if (!has) authoredNames.push_back(e.propNames[pi]);
                    }
                }
            }
            // Phase 2: resolve values strongest -> weakest.
            // over=true property in a stronger layer applies ONLY if the
            // property is authored (defined) in a weaker layer.
            // Track which properties have been resolved.
            std::vector<bool> resolved(authoredNames.size(), false);
            for (const SceneLayer& layer : layers) {
                for (const LayerEntity& e : layer.entities) {
                    if (e.id != id) continue;
                    for (std::size_t pi = 0; pi < e.propNames.size(); ++pi) {
                        const std::string& name = e.propNames[pi];
                        // Find this property in the authored list.
                        int idx = -1;
                        for (std::size_t ai = 0; ai < authoredNames.size(); ++ai) {
                            if (authoredNames[ai] == name) { idx = static_cast<int>(ai); break; }
                        }
                        if (idx < 0) continue;  // not an authored property
                        if (resolved[static_cast<std::size_t>(idx)]) continue;
                        if (e.propOver[pi]) {
                            // Over: apply only if authored below (i.e. the
                            // property is defined by a WEAKER layer — we
                            // approximate: apply over if not yet resolved AND
                            // the property was authored by a weaker layer).
                            // To keep this simple and deterministic: an over
                            // in a stronger layer applies only if the
                            // property exists as a DEFINE in any weaker
                            // layer. We detect that below.
                            if (!authored_below(layers, id, name, li_of(layers, layer))) {
                                continue;
                            }
                        }
                        composed.propNames.push_back(name);
                        composed.propValues.push_back(e.propValues[pi]);
                        resolved[static_cast<std::size_t>(idx)] = true;
                    }
                }
            }
            // Phase 3: any authored property not resolved by any layer gets
            // the weakest DEFINE value (fallback; normally resolved already).
            for (std::size_t ai = 0; ai < authoredNames.size(); ++ai) {
                if (resolved[ai]) continue;
                for (std::size_t li = layers.size(); li > 0; --li) {
                    const SceneLayer& layer = layers[li - 1];
                    for (const LayerEntity& e : layer.entities) {
                        if (e.id != id) continue;
                        for (std::size_t pi = 0; pi < e.propNames.size(); ++pi) {
                            if (e.propOver[pi]) continue;
                            if (e.propNames[pi] != authoredNames[ai]) continue;
                            composed.propNames.push_back(e.propNames[pi]);
                            composed.propValues.push_back(e.propValues[pi]);
                            resolved[ai] = true;
                            break;
                        }
                        if (resolved[ai]) break;
                    }
                    if (resolved[ai]) break;
                }
            }
            out.push_back(std::move(composed));
        }
        return true;
    }

private:
    // True if `name` is DEFINE-authored (non-over) for entity `id` in a
    // layer weaker (higher index) than `strongLayerIndex`.
    bool authored_below(const std::vector<SceneLayer>& layers,
                        const std::string& id, const std::string& name,
                        std::size_t strongLayerIndex) {
        for (std::size_t li = strongLayerIndex + 1; li < layers.size(); ++li) {
            const SceneLayer& layer = layers[li];
            for (const LayerEntity& e : layer.entities) {
                if (e.id != id) continue;
                for (std::size_t pi = 0; pi < e.propNames.size(); ++pi) {
                    if (e.propOver[pi]) continue;
                    if (e.propNames[pi] == name) return true;
                }
            }
        }
        return false;
    }

    std::size_t li_of(const std::vector<SceneLayer>& layers,
                      const SceneLayer& target) {
        for (std::size_t i = 0; i < layers.size(); ++i) {
            if (&layers[i] == &target) return i;
        }
        return layers.size();
    }
};

std::shared_ptr<ISceneLayers> create_scene_layers() {
    return std::make_shared<SceneLayers>();
}

}  // namespace assets
}  // namespace engine
