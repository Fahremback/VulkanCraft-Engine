#pragma once

#include "engine/gameplay/IGameplayIntegration.hpp"
#include "engine/ai/IAiDebugInfo.hpp"
#include "engine/rendering/IRenderingDebugView.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace engine::gameplay {

struct GameplayDebugSnapshot {
    GameplayIntegrationSnapshot integration;
    std::uint64_t aiTick{0};
    std::uint32_t renderCards{0};
    bool bindingsComplete{false};
    bool runtimeWiringComplete{false};
};

class IGameplayDebugSurface {
public:
    virtual ~IGameplayDebugSurface() = default;
    virtual void bind_integration(const IGameplayIntegration* value) = 0;
    virtual void bind_ai(const ai::IAiDebugRecorder* value) = 0;
    virtual void bind_rendering(const Engine::Rendering::IRenderingDebugView* value) = 0;
    virtual GameplayDebugSnapshot snapshot() const = 0;
    virtual std::string to_json() const = 0;
};

std::unique_ptr<IGameplayDebugSurface> create_gameplay_debug_surface();

} // namespace engine::gameplay
