#pragma once

// Deterministic world tick scheduler (META section 9).
//
// The engine owns the authoritative fixed tick clock, the ORDER of tick work,
// the budgets and the safety (dedup/cancel/active regions). The project owns
// the reactions: handlers receive the ticked cell and decide what grows, fires
// or reacts. This is the engine-neutral ordering layer block entities,
// illumination, fluids and scheduled events build on.
//
// Determinism contract: given the same seed, the same schedule_* calls, the
// same budgets and the same advance() sequence, tick work executes in the same
// order on every run and every machine — pending work is sorted (x, y, z)
// every tick, never iterated by hash order; random tick cells derive from
// (seed, chunk, tick), never from runtime state. Budget overflows carry to the
// next tick in the same sorted order: no event is lost and none reorders
// across ticks.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// One voxel coordinate of tick work.
struct TickCell {
    int x{ -1 };
    int y{ -1 };
    int z{ -1 };
    friend bool operator==(const TickCell&, const TickCell&) = default;
};

struct TickCellHash {
    std::size_t operator()(const TickCell& cell) const {
        std::size_t h = std::hash<int>{}(cell.x);
        h ^= std::hash<int>{}(cell.y) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
        h ^= std::hash<int>{}(cell.z) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
        return h;
    }
};

// A chunk key for random-tick scheduling (packed ints break on negative
// coordinates, so random work keys on a small struct instead).
struct TickChunk {
    int x{ 0 };
    int z{ 0 };
    friend bool operator==(const TickChunk&, const TickChunk&) = default;
};

struct TickChunkHash {
    std::size_t operator()(const TickChunk& chunk) const {
        std::size_t h = std::hash<int>{}(chunk.x);
        h ^= std::hash<int>{}(chunk.z) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
        return h;
    }
};

class WorldScheduler {
public:
    enum class Phase : int {
        BlockTick = 0,     // project block reactions (redstone, plants, ...)
        RandomTick = 1,    // one deterministic cell per chunk per tick
        FluidTick = 2,     // cell-driven fluid work (the world's fluid sim is
                           // driven by the fixed-tick callback, see below)
        ScheduledTick = 3, // absolute-tick deadlines (one-shot / periodic)
        NeighborTick = 4,  // ordered neighbor propagation (FALTANTES §6 item
                           // 124): a changed center notifies its orthogonal
                           // neighbors in a fixed, deterministic order
        Count = 5
    };

    using Handler = std::function<void(const TickCell&)>;
    // Neighbor phase payload: the changed `center` and the `neighbor` cell
    // being notified (center + one unit on an axis). The project decides what
    // a neighbor does (light/fluid/reaction propagation) from the fixed order.
    struct NeighborUpdate {
        TickCell center{ -1, -1, -1 };
        TickCell neighbor{ -1, -1, -1 };
        int axis{ 0 };   // 0..2
        int sign{ 0 };   // -1 or +1 along axis
        friend bool operator==(const NeighborUpdate&, const NeighborUpdate&) = default;
    };
    using NeighborHandler = std::function<void(const NeighborUpdate&)>;

    // seed drives random-tick cell selection; chunkSize matches the world's
    // chunk dimensions (default 16) for active-region and column math;
    // randomHeight bounds the deterministic random-tick y.
    explicit WorldScheduler(uint64_t seed = 1337, int chunkSize = 16,
                            int randomHeight = 128);

    // Fixed tick clock. Accumulates deltaSeconds; every full fixedStepSeconds
    // runs one tick (phases in order, then the tick callback). Returns the
    // number of ticks executed, capped so a long hitch never spirals.
    int advance(float deltaSeconds, float fixedStepSeconds);

    // ---- scheduling ----
    void schedule_block_tick(TickCell cell, int priority = 0);
    void schedule_random_tick(int chunkX, int chunkZ);
    void schedule_fluid_tick(TickCell cell);
    void schedule_scheduled_tick(TickCell cell, uint64_t atTick);
    // FALTANTES §6 item 124: queues the cell's SIX orthogonal neighbors (one
    // unit on each axis, both signs) for the NeighborTick phase. Deduped by
    // center; the handler receives one NeighborUpdate per neighbor in the
    // fixed order (-x, +x, -y, +y, -z, +z).
    void schedule_neighbor_tick(TickCell cell);
    // Removes the cell from every pending phase (including scheduled).
    void cancel_cell(TickCell cell);
    // Removes every pending cell/chunk inside the given chunk (used when the
    // world evicts a chunk: queued work must not outlive its chunk).
    void cancel_chunk(int chunkX, int chunkZ);
    // Removes the center's pending neighbor propagation.
    void cancel_neighbor(TickCell cell);

    // ---- configuration ----
    void set_handler(Phase phase, Handler handler);
    void set_neighbor_handler(NeighborHandler handler);
    // -1 = unlimited (default). Work beyond the budget carries to the next
    // tick, in the same sorted order. NeighborTick budget counts NEIGHBORS
    // dispatched (six per queued center).
    void set_budget(Phase phase, int maxPerTick);
    // Fired once after every completed tick (after all phases). The engine
    // world uses it to run its fluid simulation at the fixed cadence.
    void set_tick_callback(std::function<void(uint64_t tick)> callback);
    // Sleeping: cells whose chunk is farther than `chunks` from the center
    // chunk stay queued (sleep) and execute when the center approaches. A
    // center of (-1, -1) disables the active region (everything ticks).
    void set_active_center(int chunkX, int chunkZ);
    void set_active_radius(int chunks);

    // ---- replay / rollback / save / headless (FALTANTES §6 item 129) ----
    // The scheduler's full determinism-relevant state: the fixed-tick clock
    // (tick, accumulator) and every pending queue (block priorities, random
    // chunks, fluid cells, scheduled deadlines, neighbor centers). Captured in
    // SORTED order (never hash order) so the byte stream is deterministic and
    // two identical runs capture identical state. restore_state() rewinds the
    // clock and queues to a previous capture (replay/rollback); the engine
    // persists capture_state() with the world save and a dedicated/headless
    // server restores it on load so tick work continues exactly where the
    // saved session was.
    struct State {
        uint64_t tick{ 0 };
        float accumulator{ 0.0f };
        std::vector<std::pair<TickCell, int>> block;
        std::vector<TickChunk> randomChunks;
        std::vector<TickCell> fluid;
        std::vector<std::pair<TickCell, uint64_t>> scheduled;
        std::vector<TickCell> neighbors;
        friend bool operator==(const State&, const State&) = default;
    };

    [[nodiscard]] State capture_state() const;
    void restore_state(const State& state);

    // Versioned binary serialization of capture_state() (magic "VCWS" + u32
    // version + state). Deterministic: the same scheduler captures identical
    // bytes; deserialize accepts only the current version and returns false
    // with a diagnostic otherwise (never silently resets). Const: safe to
    // capture from the world's const save paths.
    std::vector<std::byte> serialize_state() const;
    bool deserialize_state(const std::vector<std::byte>& data,
                           std::string& errorOut);

    // ---- observability ----
    [[nodiscard]] uint64_t current_tick() const { return tick_; }
    [[nodiscard]] std::size_t pending_count() const;
    // Fluid cells awaiting a tick (fluidPending_). 0 together with a drained
    // active queue is the fluid fixed point (no work scheduled, none in
    // flight) — activeFluidCells alone is satisfiable between ticks.
    [[nodiscard]] std::size_t fluid_pending_count() const {
        return fluidPending_.size();
    }
    [[nodiscard]] std::size_t executed_count(Phase phase) const {
        return executed_[static_cast<int>(phase)];
    }

private:
    void run_tick();
    void run_phase(Phase phase);
    void run_random_phase();
    void run_scheduled_phase();
    void run_neighbor_phase();
    // Deterministic column (x, z, y) inside a chunk from seed/chunk/tick.
    [[nodiscard]] TickCell random_cell(int chunkX, int chunkZ, uint64_t tick) const;
    // True when the cell's chunk is inside the active region (or none set).
    [[nodiscard]] bool is_active(const TickCell& cell) const;
    static int floor_div(int value, int divisor);

    uint64_t seed_;
    int chunkSize_;
    int randomHeight_;
    uint64_t tick_{ 0 };
    float accumulator_{ 0.0f };
    int maxTicksPerAdvance_{ 128 };

    // Per-phase pending work. BlockTick maps cell -> priority; ScheduledTick
    // maps cell -> absolute deadline; RandomTick holds chunk keys; NeighborTick
    // holds the changed centers awaiting neighbor dispatch.
    std::unordered_map<TickCell, int, TickCellHash> blockPending_;
    std::unordered_set<TickChunk, TickChunkHash> randomChunks_;
    std::unordered_set<TickCell, TickCellHash> fluidPending_;
    std::unordered_map<TickCell, uint64_t, TickCellHash> scheduledPending_;
    std::unordered_set<TickCell, TickCellHash> neighborCenters_;

    std::function<void(const TickCell&)> handlers_[5];
    std::function<void(const NeighborUpdate&)> neighborHandler_;
    std::function<void(uint64_t)> tickCallback_;
    int budgets_[5]{ -1, -1, -1, -1, -1 };
    int activeCenterX_{ -1 };
    int activeCenterZ_{ -1 };
    int activeRadius_{ -1 };

    std::size_t executed_[5]{ 0, 0, 0, 0 };
};
