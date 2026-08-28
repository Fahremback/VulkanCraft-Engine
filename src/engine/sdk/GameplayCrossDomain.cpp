#include "engine/gameplay/IGameplayCrossDomain.hpp"

#include <iomanip>
#include <sstream>

namespace engine::gameplay {
namespace {
class CrossDomain final : public IGameplayCrossDomain {
public:
    bool bind_integration(IGameplayIntegration* value) override {
        integration_ = value;
        return value != nullptr;
    }

    bool bind_navigation(navigation::INavigationProvider* provider,
                         navigation::INavInvalidation* invalidation,
                         navigation::INavStreaming* streaming,
                         navigation::INavigationSchedulerBridge* scheduler,
                         std::string& errorOut) override {
        if (!provider || !invalidation || !streaming || !scheduler) {
            errorOut = "all navigation seams are required";
            return false;
        }
        provider_ = provider;
        invalidation_ = invalidation;
        streaming_ = streaming;
        scheduler_ = scheduler;
        errorOut.clear();
        return true;
    }

    bool bind_debug(const ai::IAiDebugRecorder* ai,
                    const Engine::Rendering::IRenderingDebugView* rendering) override {
        if (!ai || !rendering) return false;
        ai_ = ai;
        rendering_ = rendering;
        return true;
    }

    bool bind_authoring(scripting::IVisualScriptGraph* scripting,
                        semantic::ISemanticApi* semantic) override {
        if (!scripting || !semantic) return false;
        scripting_ = scripting;
        semantic_ = semantic;
        return true;
    }

    void refresh() override {
        if (rendering_) renderCards_ = rendering_->snapshot().cardCount;
        if (ai_ && ai_->snapshot()) aiTick_ = ai_->snapshot()->tick;
        if (semantic_) semanticKinds_ = semantic_->kinds().size();
    }

    GameplayCrossDomainSnapshot snapshot() const override {
        GameplayCrossDomainSnapshot value;
        if (integration_) value.integration = integration_->snapshot();
        if (provider_) value.navigationRevision = provider_->revision();
        if (invalidation_) value.invalidNavigationTiles = invalidation_->invalid_tiles().size();
        if (streaming_) value.loadedNavigationTiles = streaming_->loaded_count();
        value.aiTick = aiTick_;
        value.renderCards = renderCards_;
        value.semanticKinds = semanticKinds_;
        value.navigationBound = provider_ && invalidation_ && streaming_ && scheduler_;
        value.debugBound = ai_ && rendering_;
        value.authoringBound = scripting_ && semantic_;
        value.bindingsComplete = integration_ && integration_->snapshot().bindingsComplete;
        value.runtimeWiringComplete = integration_ && integration_->snapshot().runtimeWiringComplete;
        value.fullyBound = value.navigationBound && value.debugBound && value.authoringBound &&
                           value.bindingsComplete && value.runtimeWiringComplete;
        return value;
    }

    std::string to_json() const override {
        const auto value = snapshot();
        std::ostringstream out;
        out << std::setprecision(17)
            << "{\"tick\":" << value.integration.tick
            << ",\"entities\":" << value.integration.entities
            << ",\"navigationRevision\":" << value.navigationRevision
            << ",\"invalidNavigationTiles\":" << value.invalidNavigationTiles
            << ",\"loadedNavigationTiles\":" << value.loadedNavigationTiles
            << ",\"aiTick\":" << value.aiTick
            << ",\"renderCards\":" << value.renderCards
            << ",\"semanticKinds\":" << value.semanticKinds
            << ",\"routedEvents\":" << value.integration.routedEvents
            << ",\"navigationBound\":" << (value.navigationBound ? "true" : "false")
            << ",\"debugBound\":" << (value.debugBound ? "true" : "false")
            << ",\"authoringBound\":" << (value.authoringBound ? "true" : "false")
            << ",\"fullyBound\":" << (value.fullyBound ? "true" : "false")
            << ",\"bindingsComplete\":" << (value.bindingsComplete ? "true" : "false")
            << ",\"runtimeWiringComplete\":" << (value.runtimeWiringComplete ? "true" : "false") << "}";
        return out.str();
    }

private:
    IGameplayIntegration* integration_{nullptr};
    navigation::INavigationProvider* provider_{nullptr};
    navigation::INavInvalidation* invalidation_{nullptr};
    navigation::INavStreaming* streaming_{nullptr};
    navigation::INavigationSchedulerBridge* scheduler_{nullptr};
    const ai::IAiDebugRecorder* ai_{nullptr};
    const Engine::Rendering::IRenderingDebugView* rendering_{nullptr};
    scripting::IVisualScriptGraph* scripting_{nullptr};
    semantic::ISemanticApi* semantic_{nullptr};
    std::uint64_t aiTick_{0};
    std::uint32_t renderCards_{0};
    std::uint64_t semanticKinds_{0};
};
}

std::unique_ptr<IGameplayCrossDomain> create_gameplay_cross_domain() {
    return std::make_unique<CrossDomain>();
}
} // namespace engine::gameplay
