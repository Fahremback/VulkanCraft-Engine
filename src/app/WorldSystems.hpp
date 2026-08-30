#pragma once

// LOTE 1 — Mundo: entidades, geração, save, time-travel (A2 itens 46/47/49/54/
// 55/66/71/72/73). Integração REAL sobre o World do jogo (simulation/voxel/
// streaming/World.hpp), wireada e chamada pelo VulkanEngineApp no loop.
//
// Cada peça é um sistema concreto com consumidor real no executável e estado
// observável (std::cout/título), não só exposição de SDK.

#include "simulation/voxel/streaming/World.hpp"
#include "simulation/voxel/simulation/WorldScheduler.hpp"

#include "engine/voxel/IVoxelBlockEntity.hpp"
#include "engine/timeline/ITimelineGraph.hpp"
#include "engine/world/ITimeTravel.hpp"
#include "engine/world/ITimelinePolicy.hpp"
#include "engine/world/IWorldManager.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace app {

// Estado observável agregado dos sistemas do Lote 1, publicado por frame.
struct WorldLote1Stats {
    // 46/47 block entities
    std::size_t blockEntityCount{ 0 };
    std::uint64_t blockEntityTicks{ 0 };
    std::size_t blockEntityAttachFailures{ 0 };
    // 49 scheduler (fases e ticks processados)
    std::uint64_t schedulerTicks{ 0 };
    std::size_t schedulerPendingCells{ 0 };
    // 54 noise/biome/ore graphs
    bool noiseGraphActive{ false };
    std::uint32_t noiseColumnsScanned{ 0 };
    std::size_t oreBlocksSeen{ 0 };
    // 66 save unificado
    bool saveRoundTrip{ false };
    std::string lastSavePath;
    // 71/72/73 time travel
    std::size_t timelineNodeCount{ 0 };
    std::size_t timelineStateCount{ 0 };
    std::size_t timeTravelRewinds{ 0 };
    std::size_t mergeCount{ 0 };  // CONTA 4 item 4 — temporal branch merges
    std::string lastTimeTravelNode;
};

// Um block entity concreto do projeto: um "clock" (contador determinístico
// por célula) que ticka via BlockTick do scheduler e é persistido (framing v5).
class BlockEntityClock final : public engine::voxel::IVoxelBlockEntity {
public:
    std::string type_id() const override { return "project:clock"; }
    void on_tick(std::uint64_t worldTick) override {
        ++tickCount_;
        lastTick = worldTick;
    }
    std::uint32_t data_version() const override { return 1; }
    std::vector<std::uint8_t> serialize_state() const override {
        const auto v = static_cast<std::uint32_t>(tickCount_ & 0xffffffffu);
        return { static_cast<std::uint8_t>(v & 0xff),
                 static_cast<std::uint8_t>((v >> 8) & 0xff),
                 static_cast<std::uint8_t>((v >> 16) & 0xff),
                 static_cast<std::uint8_t>((v >> 24) & 0xff) };
    }
    bool deserialize_state(const std::vector<std::uint8_t>& data,
                           std::uint32_t version) override {
        (void)version;
        if (data.size() >= 4) {
            tickCount_ = static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(data[0]) |
                (static_cast<std::uint32_t>(data[1]) << 8) |
                (static_cast<std::uint32_t>(data[2]) << 16) |
                (static_cast<std::uint32_t>(data[3]) << 24));
        }
        return true;
    }
    std::uint64_t tickCount() const { return tickCount_; }
    std::uint64_t lastTick{ 0 };

private:
    std::uint64_t tickCount_{ 0 };
};

// Um generator override do jogo (item 54): noise/biome/ore determinístico,
// controller por seed real compartilhada com o preview (item 55).
class DataDrivenGenerator final : public engine::voxel::IVoxelGenerator {
public:
    explicit DataDrivenGenerator(std::uint64_t seed);
    engine::voxel::TerrainPoint sample(float worldX, float worldZ) const override;
    float cave_density(float wx, float wy, float wz) const override;
    float ore_density(float wx, float wy, float wz) const override;
    std::uint32_t ore_block(float oreDensity, int y,
                            std::uint32_t builtinBlock) const override;
    std::uint64_t seed() const { return seed_; }
    mutable std::atomic<std::uint32_t> columnsScanned{ 0 };
    mutable std::atomic<std::size_t> oreSeen{ 0 };

private:
    std::uint64_t seed_;
    float heightAt(float x, float z) const;
};

// Estado vivo dos sistemas — detida pelo VulkanEngineApp.
class WorldLote1 {
public:
    // 46/47/49: registrar tipo, anexar clock e alimentar scheduler. Chama no init.
    void init(World& world);
    // 46/47: anexa clocks em 3 colunas de bloco SÓLIDO perto do jogador (o attach
    // exige bloco não-Air; retry até o chunk render). Retorna quantos conseguiu.
    std::size_t try_attach_clocks(float px, float py, float pz);
    // 71/72/73: registro do timeline + time-travel sobre o manager. Chama no init.
    void init_time(std::unique_ptr<engine::world::IWorldManager>& manager);
    // Por frame no loop do jogo: amostra observáveis do mundo e roda time-travel.
    WorldLote1Stats tick() const;
    // 66: save unificado (v5) + reload round-trip. Retorna true quando OK.
    bool save_unified(const std::string& path);
    // 71/72: viajar para um estado temporal (rewind) e reportar.
    bool travel_to(const std::string& name, std::string& errorOut);
    // 71/72/73: CONTA 4 item 4 — fork a temporal state into an independent
    // branch and travel to it (a divergent timeline) over the live world.
    bool branch_and_travel(const std::string& name, const std::string& fromState,
                           std::string& errorOut);
    // CONTA 4 item 4 — merge two temporal branches back into one timeline and
    // travel the WORLD to the merge point. The merged entry is created as a
    // child of the two branches' COMMON ANCESTOR (the first shared causal
    // node, a divergent branch is re-joined) and then traveled to, so rewind,
    // branch and merge all apply to the LIVE voxel/entity world.
    bool merge_and_travel(const std::string& name, const std::string& branchA,
                          const std::string& branchB, std::string& errorOut);

    WorldLote1Stats stats{};

private:
    World* world_{ nullptr };
    std::unique_ptr<engine::timeline::ITimelineGraph> timelineGraph_;
    std::unique_ptr<engine::world::ITimeTravel> timeTravel_;
    std::unique_ptr<engine::world::ITimelinePolicy> timelinePolicy_;
    engine::world::IWorldManager* manager_{ nullptr };
    // Temporal-state-name -> timeline-graph node. Kept so a MERGE can compute
    // the common ancestor of two temporal branches and re-join them (rewind,
    // branch and merge all land on the same ITimeTravel + ITimelineGraph
    // products the game owns).
    mutable std::unordered_map<std::string, engine::timeline::TimelineNodeId>
        stateNodes_;
    // Ensures a timeline-graph node exists for a temporal state name and
    // returns its id (0 when the graph is absent). New nodes are forked from
    // the source state's node (or the root when none recorded) so a later
    // MERGE can compute their common ancestor. Thread-free (frame/init only).
    engine::timeline::TimelineNodeId RecordNodeFor(const std::string& name);
    mutable std::atomic<std::uint64_t> blockTickTotal_{ 0 };
    bool initDone_{ false };
    bool timeInitDone_{ false };
    bool clocksAttached_{ false };
    std::string lastSavePath_;
};

}  // namespace app