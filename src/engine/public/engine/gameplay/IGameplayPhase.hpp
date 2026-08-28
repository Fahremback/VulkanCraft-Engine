#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::gameplay {

enum class GameplayDomain : std::uint8_t { Ecs, Navigation, Ai, Animation, Physics, Voxel, Renderer, Multiplayer, Editor, Scripting, Audio, Worlds, ExternalSolutions };

struct GameplayDomainStatus {
    GameplayDomain domain{GameplayDomain::Ecs};
    bool producerBound{false};
    bool consumerBound{false};
    bool persistenceBound{false};
    bool replicationBound{false};
};

class IGameplayPhase {
public:
    virtual ~IGameplayPhase() = default;
    virtual bool configure(const std::vector<GameplayDomain>& domains, std::string& errorOut) = 0;
    virtual bool mark_producer_bound(GameplayDomain domain) = 0;
    virtual bool mark_consumer_bound(GameplayDomain domain) = 0;
    virtual bool mark_persistence_bound(GameplayDomain domain) = 0;
    virtual bool mark_replication_bound(GameplayDomain domain) = 0;
    virtual bool complete(std::string& errorOut) const = 0;
    virtual std::vector<GameplayDomainStatus> status() const = 0;
    virtual void reset() = 0;
};

std::unique_ptr<IGameplayPhase> create_gameplay_phase();

} // namespace engine::gameplay
