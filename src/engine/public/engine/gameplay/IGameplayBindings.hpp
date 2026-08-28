#pragma once

#include "engine/gameplay/IGameplayPhase.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::gameplay {

struct GameplayBinding {
    GameplayDomain domain{GameplayDomain::Ecs};
    std::string producerId;
    std::string consumerId;
    std::string persistenceId;
    std::string replicationId;
};

struct GameplayExternalBinding {
    std::string solutionId;
    GameplayDomain domain{GameplayDomain::ExternalSolutions};
    std::string adapterId;
    std::string contractId;
    bool runtimeOptional{true};
};

class IGameplayBindings {
public:
    virtual ~IGameplayBindings() = default;
    virtual bool configure(const std::vector<GameplayBinding>& bindings,
                           std::string& errorOut) = 0;
    virtual bool configure_external(const std::vector<GameplayExternalBinding>& bindings,
                                    std::string& errorOut) = 0;
    virtual bool bind_producer(GameplayDomain domain, const std::string& id,
                               std::string& errorOut) = 0;
    virtual bool bind_consumer(GameplayDomain domain, const std::string& id,
                               std::string& errorOut) = 0;
    virtual bool bind_persistence(GameplayDomain domain, const std::string& id,
                                  std::string& errorOut) = 0;
    virtual bool bind_replication(GameplayDomain domain, const std::string& id,
                                  std::string& errorOut) = 0;
    virtual bool complete(std::string& errorOut) const = 0;
    virtual std::vector<GameplayBinding> bindings() const = 0;
    virtual std::vector<GameplayExternalBinding> external_bindings() const = 0;
    virtual std::string to_json() const = 0;
};

std::unique_ptr<IGameplayBindings> create_gameplay_bindings();

} // namespace engine::gameplay
