#include "WorldPartition.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace Engine::World {

bool CellBounds::contains(const glm::vec3& p) const noexcept {
    return p.x >= minimum.x && p.y >= minimum.y && p.z >= minimum.z &&
           p.x <= maximum.x && p.y <= maximum.y && p.z <= maximum.z;
}

float CellBounds::distance_squared(const glm::vec3& p) const noexcept {
    const glm::vec3 q = glm::max(minimum - p, glm::max(p - maximum, glm::vec3(0.0f)));
    return glm::dot(q, q);
}

WorldPartition::WorldPartition(IWorldCellProvider& provider, float cellSize)
    : provider_(provider), cellSize_(std::max(1.0f, cellSize)) {}

bool WorldPartition::add_cell(CellDescriptor descriptor) {
    if (cells_.contains(descriptor.coordinate)) return false;
    cells_.emplace(descriptor.coordinate, RuntimeCell{std::move(descriptor)});
    return true;
}

bool WorldPartition::remove_cell(CellCoord cell) {
    auto it = cells_.find(cell);
    if (it == cells_.end() || it->second.state != CellState::Unloaded) return false;
    dependencies_.erase(cell);
    for (auto& [_, values] : dependencies_)
        std::erase(values, cell);
    cells_.erase(it);
    return true;
}

bool WorldPartition::contains(CellCoord cell) const { return cells_.contains(cell); }

std::optional<CellDescriptor> WorldPartition::descriptor(CellCoord cell) const {
    const auto it = cells_.find(cell);
    return it == cells_.end() ? std::nullopt : std::optional<CellDescriptor>{it->second.descriptor};
}

bool WorldPartition::introduces_cycle(CellCoord owner, CellCoord dependency) const {
    std::vector<CellCoord> pending{dependency};
    std::unordered_set<CellCoord, CellCoordHash> seen;
    while (!pending.empty()) {
        const CellCoord current = pending.back(); pending.pop_back();
        if (current == owner) return true;
        if (!seen.insert(current).second) continue;
        if (auto it = dependencies_.find(current); it != dependencies_.end())
            pending.insert(pending.end(), it->second.begin(), it->second.end());
    }
    return false;
}

bool WorldPartition::add_dependency(CellCoord owner, CellCoord dependency) {
    if (owner == dependency || !contains(owner) || !contains(dependency) || introduces_cycle(owner, dependency)) return false;
    auto& values = dependencies_[owner];
    if (std::find(values.begin(), values.end(), dependency) != values.end()) return false;
    values.push_back(dependency);
    return true;
}

bool WorldPartition::remove_dependency(CellCoord owner, CellCoord dependency) {
    auto it = dependencies_.find(owner);
    if (it == dependencies_.end()) return false;
    const auto removed = std::erase(it->second, dependency);
    if (it->second.empty()) dependencies_.erase(it);
    return removed != 0;
}

void WorldPartition::append_dependency_order(CellCoord cell,
        std::unordered_set<CellCoord, CellCoordHash>& visited, std::vector<CellCoord>& ordered) const {
    if (!visited.insert(cell).second) return;
    if (auto it = dependencies_.find(cell); it != dependencies_.end())
        for (const CellCoord dependency : it->second) append_dependency_order(dependency, visited, ordered);
    ordered.push_back(cell);
}

std::vector<CellCoord> WorldPartition::dependencies_of(CellCoord owner, bool transitive) const {
    const auto it = dependencies_.find(owner);
    if (it == dependencies_.end()) return {};
    if (!transitive) return it->second;
    std::unordered_set<CellCoord, CellCoordHash> visited;
    std::vector<CellCoord> result;
    append_dependency_order(owner, visited, result);
    std::erase(result, owner);
    return result;
}

std::vector<CellCoord> WorldPartition::referencers_of(CellCoord dependency) const {
    std::vector<CellCoord> result;
    for (const auto& [owner, values] : dependencies_)
        if (std::find(values.begin(), values.end(), dependency) != values.end()) result.push_back(owner);
    return result;
}

bool WorldPartition::set_streaming_source(StreamingSource source) {
    if (source.id == 0 || source.loadRadius < 0.0f || source.activationRadius < 0.0f ||
        source.activationRadius > source.loadRadius || source.unloadHysteresis < 0.0f) return false;
    sources_[source.id] = std::move(source);
    return true;
}

bool WorldPartition::remove_streaming_source(uint64_t sourceId) { return sources_.erase(sourceId) != 0; }
void WorldPartition::clear_streaming_sources() { sources_.clear(); }

std::vector<StreamingSource> WorldPartition::streaming_sources() const {
    std::vector<StreamingSource> result;
    result.reserve(sources_.size());
    for (const auto& [_, source] : sources_) result.push_back(source);
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.priority > b.priority; });
    return result;
}

void WorldPartition::set_state(RuntimeCell& cell, CellState state) {
    cell.state = state;
    if (stateCallback_) stateCallback_(cell.descriptor.coordinate, state);
}

bool WorldPartition::request_load(CellCoord coordinate, int32_t priority, bool activate) {
    auto it = cells_.find(coordinate);
    if (it == cells_.end()) return false;
    RuntimeCell& cell = it->second;
    cell.priority = std::max(cell.priority, priority);
    if (cell.state == CellState::Loaded || cell.state == CellState::Active) {
        if (activate && cell.state != CellState::Active) {
            set_state(cell, CellState::Active);
            profiler_.record({coordinate, CellProfileOperation::Activate, std::chrono::steady_clock::now(), {}, 0});
        }
        return true;
    }
    set_state(cell, CellState::Loading);
    CellPayload payload;
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    if (!provider_.load(cell.descriptor, payload, error)) {
        cell.lastError = std::move(error);
        set_state(cell, CellState::Failed);
        profiler_.record({coordinate, CellProfileOperation::Failure, std::chrono::steady_clock::now(),
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started), 0});
        return false;
    }
    const uint64_t bytes = payload.bytes.size();
    residentBytes_ += bytes;
    cell.payload = std::move(payload);
    cell.lastError.clear();
    set_state(cell, activate ? CellState::Active : CellState::Loaded);
    profiler_.record({coordinate, CellProfileOperation::Load, std::chrono::steady_clock::now(),
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started), bytes});
    if (activate) profiler_.record({coordinate, CellProfileOperation::Activate, std::chrono::steady_clock::now(), {}, 0});
    return true;
}

bool WorldPartition::has_resident_referencer(CellCoord cell,
        const std::unordered_set<CellCoord, CellCoordHash>& desired) const {
    for (const CellCoord owner : referencers_of(cell)) {
        if (desired.contains(owner)) return true;
        const auto it = cells_.find(owner);
        if (it != cells_.end() && (it->second.state == CellState::Loaded || it->second.state == CellState::Active)) return true;
    }
    return false;
}

bool WorldPartition::request_unload(CellCoord coordinate, bool force) {
    auto it = cells_.find(coordinate);
    if (it == cells_.end()) return false;
    RuntimeCell& cell = it->second;
    if (cell.state == CellState::Unloaded) return true;
    if (!force && has_resident_referencer(coordinate, {})) return false;
    if (!cell.payload) { set_state(cell, CellState::Unloaded); return true; }
    const uint64_t bytes = cell.payload->bytes.size();
    if (cell.state == CellState::Active) {
        set_state(cell, CellState::Loaded);
        profiler_.record({coordinate, CellProfileOperation::Deactivate, std::chrono::steady_clock::now(), {}, 0});
    }
    set_state(cell, CellState::Unloading);
    provider_.unload(cell.descriptor, std::move(*cell.payload));
    cell.payload.reset();
    residentBytes_ = bytes > residentBytes_ ? 0 : residentBytes_ - bytes;
    set_state(cell, CellState::Unloaded);
    profiler_.record({coordinate, CellProfileOperation::Unload, std::chrono::steady_clock::now(), {}, bytes});
    return true;
}

void WorldPartition::tick() {
    std::unordered_set<CellCoord, CellCoordHash> desired;
    std::unordered_set<CellCoord, CellCoordHash> active;
    std::unordered_map<CellCoord, int32_t, CellCoordHash> priorities;
    for (const auto& [coordinate, cell] : cells_) {
        if (cell.descriptor.alwaysLoaded) desired.insert(coordinate);
        for (const auto& [_, source] : sources_) {
            if (!source.enabled) continue;
            const float distanceSquared = cell.descriptor.bounds.distance_squared(source.position);
            const bool resident = cell.state == CellState::Loaded || cell.state == CellState::Active;
            const float radius = source.loadRadius + (resident ? source.unloadHysteresis : 0.0f);
            if (distanceSquared <= radius * radius) {
                desired.insert(coordinate);
                priorities[coordinate] = std::max(priorities[coordinate], source.priority);
            }
            if (distanceSquared <= source.activationRadius * source.activationRadius) active.insert(coordinate);
        }
    }

    std::vector<CellCoord> ordered;
    std::unordered_set<CellCoord, CellCoordHash> visited;
    for (const CellCoord cell : desired) append_dependency_order(cell, visited, ordered);
    desired = visited;
    for (const CellCoord cell : ordered)
        request_load(cell, priorities[cell], active.contains(cell));

    // Reverse dependency order guarantees owners are released before dependencies.
    for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
        auto cell = cells_.find(*it);
        if (cell != cells_.end() && cell->second.state == CellState::Active && !active.contains(*it)) {
            set_state(cell->second, CellState::Loaded);
            profiler_.record({*it, CellProfileOperation::Deactivate, std::chrono::steady_clock::now(), {}, 0});
        }
    }
    std::vector<CellCoord> unload;
    for (const auto& [coordinate, cell] : cells_)
        if (!desired.contains(coordinate) && (cell.state == CellState::Loaded || cell.state == CellState::Active))
            unload.push_back(coordinate);
    bool progress = true;
    while (progress && !unload.empty()) {
        progress = false;
        for (auto it = unload.begin(); it != unload.end();) {
            if (!has_resident_referencer(*it, desired) && request_unload(*it)) { it = unload.erase(it); progress = true; }
            else ++it;
        }
    }
}

std::vector<CellRuntimeSnapshot> WorldPartition::runtime_snapshot() const {
    std::vector<CellRuntimeSnapshot> result;
    result.reserve(cells_.size());
    for (const auto& [coordinate, cell] : cells_)
        result.push_back({coordinate, cell.state, cell.payload ? cell.payload->bytes.size() : 0, cell.priority, cell.lastError});
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.coordinate < b.coordinate; });
    return result;
}

} // namespace Engine::World
