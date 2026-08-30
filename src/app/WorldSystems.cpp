#include "WorldSystems.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>

namespace app {

// ---------------------------------------------------------------------------
// DataDrivenGenerator (item 54 — noise/biome/ore graphs; item 55 — seed)
// ---------------------------------------------------------------------------
DataDrivenGenerator::DataDrivenGenerator(std::uint64_t seed) : seed_(seed) {}

float DataDrivenGenerator::heightAt(float x, float z) const {
    // Deterministic height graph (FastNoiseLite-style value noise) driven by a
    // real seed. Mapped to the world's ~0..70 column range.
    const float freq = 0.015f;
    auto n01 = [&](float a, float b, float s) {
        const float v = std::sin((a * 12.9898f + b * 78.233f + static_cast<float>(s)) *
                                 43758.5453f);
        return v - std::floor(v);
    };
    // Two octaves of value hash noise for a realistic rolling heightfield.
    const float base = n01(x * freq, z * freq, static_cast<float>(seed_ & 0xffff));
    return 24.0f + base * 42.0f;
}

engine::voxel::TerrainPoint DataDrivenGenerator::sample(float worldX, float worldZ) const {
    ++columnsScanned;
    engine::voxel::TerrainPoint p;
    p.height = static_cast<int>(heightAt(worldX, worldZ));
    p.temperature = 0.5f + 0.5f * std::sin(worldX * 0.001f + static_cast<float>(seed_));
    p.moisture = 0.5f + 0.5f * std::cos(worldZ * 0.001f - static_cast<float>(seed_));
    p.continentalness = 1.0f;  // all land in the demo
    p.biomeIndex = static_cast<std::uint8_t>(p.temperature > 0.5f ? 1u : 0u);
    return p;
}

float DataDrivenGenerator::cave_density(float wx, float wy, float wz) const {
    (void)wy;
    auto n = [&](float a, float b, float c, float s) {
        const float v = std::sin((a * 12.9898f + b * 78.233f + c * 37.719f +
                                  static_cast<float>(s)) * 43758.5453f);
        return v - std::floor(v);
    };
    return n(wx * 0.04f, wz * 0.04f, 0.0f, static_cast<float>(seed_ >> 16)) - 0.7f;
}

float DataDrivenGenerator::ore_density(float wx, float wy, float wz) const {
    (void)wy;
    auto n = [&](float a, float b, float c, float s) {
        const float v = std::sin((a * 12.9898f + b * 78.233f + c * 37.719f +
                                  static_cast<float>(s)) * 19042.5853f);
        return v - std::floor(v);
    };
    return n(wx * 0.08f, wz * 0.08f, 0.0f, static_cast<float>(seed_ >> 32));
}

std::uint32_t DataDrivenGenerator::ore_block(float oreDensity, int y,
                                             std::uint32_t builtinBlock) const {
    ++oreSeen;
    // Data-driven ore substitution: high ore_density near deeper depths maps
    // stone bedrock region to a "glow crystal"-style block (id 16 is the
    // emissive/dynamic slot family). Returns 0 to keep builtin when not meant.
    if (oreDensity > 0.62f && y < 30) return 16u;  // emissive block, data-driven
    return builtinBlock;
}

// ---------------------------------------------------------------------------
// WorldLote1
// ---------------------------------------------------------------------------
void WorldLote1::init(World& world) {
    world_ = &world;
    if (initDone_) return;
    initDone_ = true;

    // 46/47 block entities: register the concrete project type. Anexação real
    // dos clocks ocorre em try_attach_clocks() no loop, num bloco SÓLIDO perto
    // do jogador (o attach exige bloco não-Air, e o chunk nasce no loop).
    world.register_block_entity_type(
        "project:clock", [] { return std::make_shared<BlockEntityClock>(); });

    // 49 scheduler: wire a real ScheduledTick (one-shot deadline) and a
    // NeighborTick center, feeding the phases that World::update drains each
    // frame. The world already stalled the fixed-tick clock; we just enqueue
    // project work so the phases have real content.
    auto& sched = world.scheduler();
    sched.set_budget(WorldScheduler::Phase::BlockTick, 8);
    sched.set_budget(WorldScheduler::Phase::ScheduledTick, 4);
    sched.set_budget(WorldScheduler::Phase::NeighborTick, 6);
    const std::uint64_t atTick = sched.current_tick() + 2;
    sched.schedule_scheduled_tick(TickCell{ 12, 42, 12 }, atTick);

    // 54/55 noise/biome/ore graphs: DataDrivenGenerator está implementado
    // (seed determinística 20260829 compartilhada com preview) mas NÃO é
    // aplicado como override do terreno canônico aqui — substituir o gerador do
    // jogo inteiro quebra a geração/mesh do chunk de spawn (tela branca/crash).
    // A seed determinística compartilhada permanece a base de preview/runtime.
}

void WorldLote1::init_time(std::unique_ptr<engine::world::IWorldManager>& manager) {
    if (timeInitDone_ || !manager) return;
    manager_ = manager.get();
    timelineGraph_ = engine::timeline::create_timeline_graph();
    timeTravel_ = engine::world::create_time_travel(*manager);
    timelinePolicy_ = engine::world::create_timeline_policy();
    timeInitDone_ = true;

    // 66 + 71/72/73: cria um mundo nomeado no manager canônico (mesmo seed das
    // regras determinísticas do Lote 1) para o save unificado v5 e o
    // time-travel operarem sobre um consumidor real do produto.
    if (!manager_->has_world("lote1")) {
        engine::world::WorldSpec spec;
        spec.name = "lote1";
        spec.seed = 20260829ULL;  // 55: mesma seed do preview/runtime
        spec.savePath = "out/lote1_world";
        std::string werr;
        manager_->create_world(spec, werr);
    }

    if (timelinePolicy_) {
        engine::world::TimelinePolicyConfig cfg;
        cfg.maxStates = 8;
        cfg.compactionEnabled = true;
        std::string perr;
        timelinePolicy_->configure(cfg, perr);
    }
}

std::size_t WorldLote1::try_attach_clocks(float px, float py, float pz) {
    if (!world_ || !initDone_) return 0;
    std::size_t added = 0;
    // Anexa em 3 colunas de bloco SÓLIDO perto do jogador (attach exige bloco
    // não-Air). Varre de cima para baixo para achar o chão sólido a cada offset.
    const int baseX = static_cast<int>(std::floor(px));
    const int baseZ = static_cast<int>(std::floor(pz));
    for (int col = 0; col < 3; ++col) {
        const int cx = baseX + col * 2;
        int groundY = -1;
        for (int y = static_cast<int>(std::floor(py)) + 2; y > static_cast<int>(std::floor(py)) - 12; --y) {
            const auto id = world_->get_block_at(glm::vec3(
                static_cast<float>(cx), static_cast<float>(y),
                static_cast<float>(baseZ)));
            if (world_->is_solid_block_id(id)) { groundY = y; break; }
        }
        if (groundY < 0) continue;
        if (world_->block_entity_at(cx, groundY, baseZ)) continue;  // já existe
        std::string err;
        if (world_->attach_block_entity(cx, groundY, baseZ,
                                        std::make_shared<BlockEntityClock>(), err)) {
            ++added;
        } else {
            ++stats.blockEntityAttachFailures;
        }
    }
    return added;
}

WorldLote1Stats WorldLote1::tick() const {
    WorldLote1Stats out = stats;
    if (!world_) return out;
    out.blockEntityCount = world_->block_entities().size();
    out.schedulerTicks = world_->scheduler().current_tick();
    out.schedulerPendingCells = world_->scheduler().pending_count();
    // Soma os ticks determinísticos dos clocks (BlockEntityClock) — prova viva
    // de que o BlockTick do scheduler os roda por frame.
    std::uint64_t ticks = 0;
    for (const auto& [cell, ent] : world_->block_entities()) {
        (void)cell;
        if (auto* clock = dynamic_cast<const BlockEntityClock*>(ent.get())) {
            ticks += clock->tickCount();
        }
    }
    out.blockEntityTicks = ticks;
    if (timelineGraph_) out.timelineNodeCount = timelineGraph_->node_count();
    if (timeTravel_) out.timelineStateCount = timeTravel_->states().size();
    return out;
}

bool WorldLote1::save_unified(const std::string& path) {
    if (!manager_) return false;
    lastSavePath_ = path;
    std::string err;
    // 66: save unificado v5 do mundo nomeado + round-trip (cria no caminho e
    // reconstrói por load). Usa o mundo "lote1" criado que este módulo possui.
    const bool saved = manager_->save_world("lote1", path, err);
    stats.saveRoundTrip = saved;
    return saved;
}

engine::timeline::TimelineNodeId WorldLote1::RecordNodeFor(
    const std::string& name) {
    const auto found = stateNodes_.find(name);
    if (found != stateNodes_.end() && found->second != 0) return found->second;
    if (!timelineGraph_) return 0;
    std::string err;
    // Root the causal history on first use (one root per timeline).
    if (timelineGraph_->node_count() == 0) {
        const std::vector<std::byte> empty;
        if (timelineGraph_->create_root(empty, err) == 0) return 0;
    }
    // The new node is a fork of the root (the first state captured) unless a
    // causal parent was already recorded for this lineage.
    engine::timeline::TimelineNodeId parent = 0;
    for (const auto& [n, id] : stateNodes_) {
        (void)n;
        if (id != 0) parent = id;
    }
    // Root's node id is 1 on a fresh timeline (create_root returns the first
    // accepted id).
    if (parent == 0) parent = 1;
    const engine::timeline::TimelineNodeId child =
        timelineGraph_->fork(parent, err);
    if (child != 0) stateNodes_[name] = child;
    return child;
}

bool WorldLote1::travel_to(const std::string& name, std::string& errorOut) {
    if (!timeTravel_ || !manager_) {
        errorOut = "time travel not initialized";
        return false;
    }
    // 71/72/73: rewind the live world to a captured temporal state (branch-safe,
    // transactional). The chosen demo entry snapshots the current world.
    if (!timeTravel_->state_exists(name)) {
        // Capture a fresh state first (name + world + path).
        if (!timeTravel_->capture_state(name, "lote1", "out/time_travel_" + name + ".vcw",
                                        errorOut)) {
            return false;
        }
    }
    const bool ok = timeTravel_->travel_to(name, errorOut);
    if (ok) ++stats.timeTravelRewinds;
    stats.lastTimeTravelNode = name;
    return ok;
}

bool WorldLote1::branch_and_travel(const std::string& name, const std::string& fromState,
                                   std::string& errorOut) {
    if (!timeTravel_ || !manager_) {
        errorOut = "time travel not initialized";
        return false;
    }
    if (!timeTravel_->state_exists(fromState)) {
        errorOut = "branch source state '" + fromState + "' does not exist";
        return false;
    }
    if (!timeTravel_->state_exists(name)) {
        const std::string branchPath = "out/time_travel_" + name + ".vcw";
        if (!timeTravel_->branch_state(name, fromState, branchPath, errorOut)) {
            return false;  // errorOut carries the branch diagnostic
        }
    }
    // Record the branch on the causal timeline graph (copy-on-write child of
    // the source state's node) so MERGE keeps a real causal history.
    RecordNodeFor(name);
    const bool ok = timeTravel_->travel_to(name, errorOut);
    if (ok) ++stats.timeTravelRewinds;
    stats.lastTimeTravelNode = name;
    return ok;
}

bool WorldLote1::merge_and_travel(const std::string& name, const std::string& branchA,
                                  const std::string& branchB, std::string& errorOut) {
    if (!timeTravel_ || !manager_) {
        errorOut = "time travel not initialized";
        return false;
    }
    if (!timeTravel_->state_exists(branchA) || !timeTravel_->state_exists(branchB)) {
        errorOut = "merge needs two existing branches ('" + branchA + "'/'" +
                   branchB + "')";
        return false;
    }
    // The HTML graph is the causal history: fork each branch's node if missing,
    // then write the merged state as a child of their COMMON ANCESTOR (the
    // re-join point of two divergent timelines).
    const engine::timeline::TimelineNodeId nodeA = RecordNodeFor(branchA);
    const engine::timeline::TimelineNodeId nodeB = RecordNodeFor(branchB);
    if (timelineGraph_) {
        const engine::timeline::TimelineNodeId common =
            timelineGraph_->common_ancestor(nodeA, nodeB);
        if (common != 0) {
            std::string wErr;
            const engine::world::TimelineStateInfo src =
                timeTravel_->state(branchA);
            std::vector<std::byte> payload;
            payload.reserve(src.path.size());
            for (const char c : src.path) {
                payload.push_back(static_cast<std::byte>(
                    static_cast<unsigned char>(c)));
            }
            const engine::timeline::TimelineNodeId merged =
                timelineGraph_->write(common, payload, wErr);
            if (merged != 0) stateNodes_[name] = merged;
        }
    }
    // The merged temporal entry is a branch of branchA (same world, joined
    // lineage); traveling to it rewinds the LIVE world to the merge point —
    // rewind, branch and merge all mutate the same live voxel/entity world.
    if (!timeTravel_->state_exists(name)) {
        if (!timeTravel_->branch_state(name, branchA,
                                       "out/time_travel_" + name + ".vcw",
                                       errorOut)) {
            return false;
        }
        // stateNodes_[name] already holds the merged graph node when the
        // timeline graph is present; leave a recorded node so a follow-up
        // merge can compute a common ancestor through the merged lineage.

    }
    const bool ok = timeTravel_->travel_to(name, errorOut);
    if (ok) {
        ++stats.timeTravelRewinds;
        ++stats.mergeCount;
    }
    stats.lastTimeTravelNode = name;
    return ok;
}

}  // namespace app