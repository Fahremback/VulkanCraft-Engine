#include "engine/gameplay/IGameplaySystemWiring.hpp"

#include <sstream>

namespace engine::gameplay {
namespace {
class SystemWiring final : public IGameplaySystemWiring {
public:
    bool attach(const GameplaySystemWiring& value, std::string& errorOut) override {
        if (!complete_seams(value)) {
            errorOut = "all gameplay system seams are required";
            return false;
        }
        wiring_ = value;
        errorOut.clear();
        return true;
    }

    bool attach_bindings(IGameplayBindings* value) override {
        bindings_ = value;
        return value != nullptr;
    }

    bool complete(std::string& errorOut) const override {
        if (!complete_seams(wiring_)) {
            errorOut = "concrete gameplay wiring is incomplete";
            return false;
        }
        if (!bindings_) {
            errorOut = "gameplay binding manifest is missing";
            return false;
        }
        if (!bindings_->complete(errorOut)) return false;
        errorOut.clear();
        return true;
    }

    const GameplaySystemWiring& wiring() const override { return wiring_; }

    std::string to_json() const override {
        std::string error;
        const bool valid = complete(error);
        std::ostringstream out;
        out << "{\"complete\":" << (valid ? "true" : "false")
            << ",\"error\":\"" << error << "\"}";
        return out.str();
    }

private:
    static bool complete_seams(const GameplaySystemWiring& value) {
        return value.ecs && value.archetypes && value.lifecycle && value.spatialIndex &&
               value.navigation && value.navigationInvalidation && value.navigationStreaming &&
               value.navigationQueries && value.physicsGameplay && value.abilities &&
               value.effectStacks && value.abilityEffects && value.dayNight &&
               value.hitReaction && value.animationLod && value.animationBudget &&
               value.animationCore && value.skinning && value.ragdollAsset && value.voxel &&
               value.multiplayer && value.renderer && value.scriptingRuntime && value.semantic &&
               value.audio && value.eventRouter && value.worlds && value.portals &&
               value.timelinePolicy && value.replay;
    }

    GameplaySystemWiring wiring_{};
    IGameplayBindings* bindings_{nullptr};
};
}

std::unique_ptr<IGameplaySystemWiring> create_gameplay_system_wiring() {
    return std::make_unique<SystemWiring>();
}

} // namespace engine::gameplay
