#include "WorldPartitionProfiler.hpp"
#include <algorithm>

namespace Engine::World {

void WorldPartitionProfiler::record(CellProfileEvent event) {
    std::scoped_lock lock(mutex_);
    switch (event.operation) {
    case CellProfileOperation::Load:
        ++profile_.loads;
        profile_.residentBytes += event.residentBytes;
        profile_.peakResidentBytes = std::max(profile_.peakResidentBytes, profile_.residentBytes);
        profile_.totalLoadTime += event.duration;
        break;
    case CellProfileOperation::Unload:
        ++profile_.unloads;
        profile_.residentBytes = event.residentBytes > profile_.residentBytes
            ? 0 : profile_.residentBytes - event.residentBytes;
        break;
    case CellProfileOperation::Failure:
        ++profile_.failures;
        break;
    case CellProfileOperation::Activate:
    case CellProfileOperation::Deactivate:
        break;
    }
    if (eventCapacity_ == 0) return;
    if (profile_.recentEvents.size() == eventCapacity_) profile_.recentEvents.erase(profile_.recentEvents.begin());
    profile_.recentEvents.push_back(std::move(event));
}

WorldPartitionProfile WorldPartitionProfiler::snapshot() const {
    std::scoped_lock lock(mutex_);
    return profile_;
}

void WorldPartitionProfiler::reset() {
    std::scoped_lock lock(mutex_);
    profile_ = {};
}

ScopedCellProfile::ScopedCellProfile(WorldPartitionProfiler& profiler, CellCoord cell,
                                     CellProfileOperation operation, uint64_t residentBytes)
    : profiler_(profiler), cell_(cell), operation_(operation), residentBytes_(residentBytes),
      started_(std::chrono::steady_clock::now()) {}

ScopedCellProfile::~ScopedCellProfile() {
    const auto now = std::chrono::steady_clock::now();
    profiler_.record({cell_, operation_, now,
                      std::chrono::duration_cast<std::chrono::microseconds>(now - started_), residentBytes_});
}

} // namespace Engine::World
