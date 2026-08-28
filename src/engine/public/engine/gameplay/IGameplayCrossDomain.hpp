#pragma once

#include "engine/gameplay/IGameplayIntegration.hpp"
#include "engine/navigation/INavigationProvider.hpp"
#include "engine/navigation/INavInvalidation.hpp"
#include "engine/navigation/INavStreaming.hpp"
#include "engine/navigation/INavigationSchedulerBridge.hpp"
#include "engine/ai/IAiDebugInfo.hpp"
#include "engine/rendering/IRenderingDebugView.hpp"
#include "engine/scripting/IVisualScriptGraph.hpp"
#include "engine/semantic/ISemanticApi.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace engine::gameplay {

struct GameplayCrossDomainSnapshot {
    GameplayIntegrationSnapshot integration;
    std::uint64_t navigationRevision{0};
    std::size_t invalidNavigationTiles{0};
    std::size_t loadedNavigationTiles{0};
    std::uint64_t aiTick{0};
    std::uint32_t renderCards{0};
    std::uint64_t semanticKinds{0};
    bool navigationBound{false};
    bool debugBound{false};
    bool authoringBound{false};
    bool fullyBound{false};
    bool bindingsComplete{false};
    bool runtimeWiringComplete{false};
};

class IGameplayCrossDomain {
public:
    virtual ~IGameplayCrossDomain() = default;
    virtual bool bind_integration(IGameplayIntegration* integration) = 0;
    virtual bool bind_navigation(navigation::INavigationProvider* provider,
                                 navigation::INavInvalidation* invalidation,
                                 navigation::INavStreaming* streaming,
                                 navigation::INavigationSchedulerBridge* scheduler,
                                 std::string& errorOut) = 0;
    virtual bool bind_debug(const ai::IAiDebugRecorder* ai,
                            const Engine::Rendering::IRenderingDebugView* rendering) = 0;
    virtual bool bind_authoring(scripting::IVisualScriptGraph* scripting,
                                semantic::ISemanticApi* semantic) = 0;
    virtual void refresh() = 0;
    virtual GameplayCrossDomainSnapshot snapshot() const = 0;
    virtual std::string to_json() const = 0;
};

std::unique_ptr<IGameplayCrossDomain> create_gameplay_cross_domain();

} // namespace engine::gameplay
