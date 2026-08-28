#include "engine/gameplay/IGameplayDebugSurface.hpp"

#include <iomanip>
#include <sstream>

namespace engine::gameplay {
namespace {
class DebugSurface final : public IGameplayDebugSurface {
public:
    void bind_integration(const IGameplayIntegration* value) override { integration_ = value; }
    void bind_ai(const ai::IAiDebugRecorder* value) override { ai_ = value; }
    void bind_rendering(const Engine::Rendering::IRenderingDebugView* value) override { rendering_ = value; }

    GameplayDebugSnapshot snapshot() const override {
        GameplayDebugSnapshot result;
        if (integration_) result.integration = integration_->snapshot();
        if (ai_ && ai_->snapshot()) result.aiTick = ai_->snapshot()->tick;
        if (rendering_) result.renderCards = rendering_->snapshot().cardCount;
        result.bindingsComplete = result.integration.bindingsComplete;
        result.runtimeWiringComplete = result.integration.runtimeWiringComplete;
        return result;
    }

    std::string to_json() const override {
        const auto value = snapshot();
        std::ostringstream out;
        out << std::setprecision(17)
            << "{\"tick\":" << value.integration.tick
            << ",\"entities\":" << value.integration.entities
            << ",\"pendingEvents\":" << value.integration.pendingEvents
            << ",\"queuedQueries\":" << value.integration.queuedQueries
            << ",\"mappedAudioEvents\":" << value.integration.mappedAudioEvents
            << ",\"routedEvents\":" << value.integration.routedEvents
            << ",\"simulationSeconds\":" << value.integration.simulationSeconds
            << ",\"aiTick\":" << value.aiTick
            << ",\"renderCards\":" << value.renderCards
            << ",\"bindingsComplete\":" << (value.bindingsComplete ? "true" : "false")
            << ",\"runtimeWiringComplete\":" << (value.runtimeWiringComplete ? "true" : "false") << "}";
        return out.str();
    }

private:
    const IGameplayIntegration* integration_{nullptr};
    const ai::IAiDebugRecorder* ai_{nullptr};
    const Engine::Rendering::IRenderingDebugView* rendering_{nullptr};
};
}

std::unique_ptr<IGameplayDebugSurface> create_gameplay_debug_surface() {
    return std::make_unique<DebugSurface>();
}
} // namespace engine::gameplay
