#pragma once

#include "WorldPartitionProfiler.hpp"
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace Engine::World {

class WorldPartition final {
public:
    using StateCallback = std::function<void(CellCoord, CellState)>;

    explicit WorldPartition(IWorldCellProvider& provider, float cellSize = 256.0f);

    [[nodiscard]] bool add_cell(CellDescriptor descriptor);
    [[nodiscard]] bool remove_cell(CellCoord cell);
    [[nodiscard]] bool contains(CellCoord cell) const;
    [[nodiscard]] std::optional<CellDescriptor> descriptor(CellCoord cell) const;

    // The owner depends on dependency. Dependencies are loaded first and retained
    // while any resident owner needs them. Cycles and unknown cells are rejected.
    [[nodiscard]] bool add_dependency(CellCoord owner, CellCoord dependency);
    [[nodiscard]] bool remove_dependency(CellCoord owner, CellCoord dependency);
    [[nodiscard]] std::vector<CellCoord> dependencies_of(CellCoord owner, bool transitive = false) const;
    [[nodiscard]] std::vector<CellCoord> referencers_of(CellCoord dependency) const;

    [[nodiscard]] bool set_streaming_source(StreamingSource source);
    [[nodiscard]] bool remove_streaming_source(uint64_t sourceId);
    void clear_streaming_sources();
    [[nodiscard]] std::vector<StreamingSource> streaming_sources() const;

    // Evaluates all sources, loads dependency closures in dependency order, activates
    // close cells, then unloads cells outside the hysteresis radius in reverse order.
    void tick();
    [[nodiscard]] bool request_load(CellCoord cell, int32_t priority = 0, bool activate = true);
    [[nodiscard]] bool request_unload(CellCoord cell, bool force = false);

    [[nodiscard]] std::vector<CellRuntimeSnapshot> runtime_snapshot() const;
    [[nodiscard]] uint64_t resident_bytes() const noexcept { return residentBytes_; }
    [[nodiscard]] float cell_size() const noexcept { return cellSize_; }
    WorldPartitionProfiler& profiler() noexcept { return profiler_; }
    const WorldPartitionProfiler& profiler() const noexcept { return profiler_; }
    void set_state_callback(StateCallback callback) { stateCallback_ = std::move(callback); }

private:
    struct RuntimeCell {
        CellDescriptor descriptor;
        CellState state{CellState::Unloaded};
        std::optional<CellPayload> payload;
        int32_t priority{};
        std::string lastError;
    };

    void set_state(RuntimeCell& cell, CellState state);
    [[nodiscard]] bool introduces_cycle(CellCoord owner, CellCoord dependency) const;
    void append_dependency_order(CellCoord cell, std::unordered_set<CellCoord, CellCoordHash>& visited,
                                 std::vector<CellCoord>& ordered) const;
    [[nodiscard]] bool has_resident_referencer(CellCoord cell,
                                               const std::unordered_set<CellCoord, CellCoordHash>& desired) const;

    IWorldCellProvider& provider_;
    float cellSize_;
    uint64_t residentBytes_{};
    std::unordered_map<CellCoord, RuntimeCell, CellCoordHash> cells_;
    std::unordered_map<CellCoord, std::vector<CellCoord>, CellCoordHash> dependencies_;
    std::unordered_map<uint64_t, StreamingSource> sources_;
    WorldPartitionProfiler profiler_;
    StateCallback stateCallback_;
};

} // namespace Engine::World
