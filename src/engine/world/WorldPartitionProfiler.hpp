#pragma once

#include "WorldTypes.hpp"
#include <chrono>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Engine::World {

enum class CellProfileOperation : uint8_t { Load, Activate, Deactivate, Unload, Failure };

struct CellProfileEvent {
    CellCoord cell;
    CellProfileOperation operation{};
    std::chrono::steady_clock::time_point timestamp;
    std::chrono::microseconds duration{};
    uint64_t residentBytes{};
};

struct WorldPartitionProfile {
    uint64_t loads{};
    uint64_t unloads{};
    uint64_t failures{};
    uint64_t residentBytes{};
    uint64_t peakResidentBytes{};
    std::chrono::microseconds totalLoadTime{};
    std::vector<CellProfileEvent> recentEvents;
};

class WorldPartitionProfiler final {
public:
    explicit WorldPartitionProfiler(size_t eventCapacity = 512) : eventCapacity_(eventCapacity) {}
    void record(CellProfileEvent event);
    [[nodiscard]] WorldPartitionProfile snapshot() const;
    void reset();

private:
    mutable std::mutex mutex_;
    size_t eventCapacity_;
    WorldPartitionProfile profile_;
};

class ScopedCellProfile final {
public:
    ScopedCellProfile(WorldPartitionProfiler& profiler, CellCoord cell, CellProfileOperation operation,
                      uint64_t residentBytes = 0);
    ~ScopedCellProfile();
    ScopedCellProfile(const ScopedCellProfile&) = delete;
    ScopedCellProfile& operator=(const ScopedCellProfile&) = delete;
private:
    WorldPartitionProfiler& profiler_;
    CellCoord cell_;
    CellProfileOperation operation_;
    uint64_t residentBytes_;
    std::chrono::steady_clock::time_point started_;
};

} // namespace Engine::World
