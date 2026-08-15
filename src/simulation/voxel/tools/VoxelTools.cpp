#include "VoxelTools.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace Engine::Voxel {
namespace {
using Change = std::pair<uint32_t, std::pair<VoxelValue, VoxelValue>>;

bool inside_shape(Int3 position, const VoxelBrushOperation& operation, bool horizontalOnly = false) {
    const glm::vec3 delta = glm::vec3(static_cast<float>(position.x), static_cast<float>(position.y), static_cast<float>(position.z)) - operation.position;
    if (operation.shape == VoxelBrushShape::Sphere) {
        const float squared = delta.x * delta.x + delta.z * delta.z + (horizontalOnly ? 0.0f : delta.y * delta.y);
        return squared <= operation.radius * operation.radius;
    }
    return std::abs(delta.x) <= operation.halfExtents.x &&
           std::abs(delta.z) <= operation.halfExtents.z &&
           (horizontalOnly || std::abs(delta.y) <= operation.halfExtents.y);
}

VoxelValue operation_value(const VoxelBrushOperation& operation) {
    VoxelValue value{operation.voxelType, operation.material, operation.density};
    return value.empty() ? VoxelValue::air() : value;
}

uint64_t voxel_key(VoxelValue value) {
    return static_cast<uint64_t>(value.type) |
           (static_cast<uint64_t>(value.material) << 16u) |
           (static_cast<uint64_t>(value.density) << 32u);
}
VoxelValue voxel_from_key(uint64_t key) {
    return {static_cast<uint16_t>(key), static_cast<uint16_t>(key >> 16u), static_cast<uint8_t>(key >> 32u)};
}

VoxelValue smoothed_value(const VoxelStructure& source, Int3 center) {
    std::map<uint64_t, uint32_t> frequency;
    uint32_t occupied = 0;
    for (int32_t z = center.z - 1; z <= center.z + 1; ++z)
        for (int32_t y = center.y - 1; y <= center.y + 1; ++y)
            for (int32_t x = center.x - 1; x <= center.x + 1; ++x) {
                if (x == center.x && y == center.y && z == center.z) continue;
                const Int3 p{x, y, z};
                if (!source.contains(p)) continue;
                const VoxelValue neighbor = source.get(p);
                if (neighbor.empty()) continue;
                ++occupied;
                ++frequency[voxel_key(neighbor)];
            }
    if (occupied < 14 || frequency.empty()) return VoxelValue::air();
    const auto winner = std::max_element(frequency.begin(), frequency.end(), [](const auto& left, const auto& right) {
        return left.second < right.second;
    });
    return voxel_from_key(winner->first);
}

std::vector<Change> evaluate(const VoxelStructure& target, const VoxelBrushOperation& operation,
                             const std::optional<VoxelSelection>& selection) {
    if (operation.radius < 0.0f || operation.halfExtents.x < 0.0f ||
        operation.halfExtents.y < 0.0f || operation.halfExtents.z < 0.0f)
        throw std::invalid_argument("Voxel brush size cannot be negative");

    std::vector<Change> changes;
    const VoxelValue desired = operation_value(operation);
    const Int3 size = target.size();
    for (int32_t z = 0; z < size.z; ++z)
        for (int32_t y = 0; y < size.y; ++y)
            for (int32_t x = 0; x < size.x; ++x) {
                const Int3 position{x, y, z};
                if (selection && !selection->contains(position)) continue;
                const bool flatten = operation.mode == VoxelBrushMode::Flatten;
                if (!inside_shape(position, operation, flatten)) continue;
                const VoxelValue before = target.get(position);
                VoxelValue after = before;
                switch (operation.mode) {
                case VoxelBrushMode::Add:
                    if (before.empty()) after = desired;
                    break;
                case VoxelBrushMode::Remove:
                    after = VoxelValue::air();
                    break;
                case VoxelBrushMode::Replace:
                    if (!before.empty()) after = desired;
                    break;
                case VoxelBrushMode::Paint:
                    if (!before.empty()) { after.material = operation.material; }
                    break;
                case VoxelBrushMode::Smooth:
                    after = smoothed_value(target, position);
                    break;
                case VoxelBrushMode::Flatten:
                    after = position.y <= operation.flattenHeight ? desired : VoxelValue::air();
                    break;
                }
                if (after.empty()) after = VoxelValue::air();
                if (before == after) continue;
                changes.push_back({static_cast<uint32_t>(target.linear_index(position)), {before, after}});
            }
    return changes;
}
} // namespace

size_t VoxelDiff::changed_voxels() const noexcept {
    size_t count = 0;
    for (const Run& run : runs_) count += run.before.size();
    return count;
}
size_t VoxelDiff::storage_bytes() const noexcept {
    size_t bytes = 0;
    for (const Run& run : runs_)
        bytes += sizeof(run.firstIndex) + (run.before.size() + run.after.size()) * sizeof(VoxelValue);
    return bytes;
}
void VoxelDiff::undo(VoxelStructure& target) const {
    for (const Run& run : runs_)
        for (size_t i = 0; i < run.before.size(); ++i)
            target.set(target.position_from_index(static_cast<size_t>(run.firstIndex) + i), run.before[i]);
}
void VoxelDiff::redo(VoxelStructure& target) const {
    for (const Run& run : runs_)
        for (size_t i = 0; i < run.after.size(); ++i)
            target.set(target.position_from_index(static_cast<size_t>(run.firstIndex) + i), run.after[i]);
}
VoxelDiff VoxelDiff::from_changes(std::vector<Change> changes) {
    std::sort(changes.begin(), changes.end(), [](const Change& left, const Change& right) { return left.first < right.first; });
    VoxelDiff result;
    for (const Change& change : changes) {
        if (result.runs_.empty() ||
            change.first != result.runs_.back().firstIndex + result.runs_.back().before.size()) {
            result.runs_.push_back({change.first, {}, {}});
        }
        result.runs_.back().before.push_back(change.second.first);
        result.runs_.back().after.push_back(change.second.second);
    }
    return result;
}

VoxelSelection::VoxelSelection(Int3 first, Int3 second)
    : minimum_{std::min(first.x, second.x), std::min(first.y, second.y), std::min(first.z, second.z)},
      maximum_{std::max(first.x, second.x), std::max(first.y, second.y), std::max(first.z, second.z)} {}
bool VoxelSelection::contains(Int3 p) const noexcept {
    return p.x >= minimum_.x && p.y >= minimum_.y && p.z >= minimum_.z &&
           p.x <= maximum_.x && p.y <= maximum_.y && p.z <= maximum_.z;
}
VoxelStructure VoxelSelection::extract(const VoxelStructure& source, std::string name) const {
    const Int3 dimensions{maximum_.x - minimum_.x + 1, maximum_.y - minimum_.y + 1, maximum_.z - minimum_.z + 1};
    VoxelStructure result(dimensions, std::move(name));
    for (int32_t z = 0; z < dimensions.z; ++z)
        for (int32_t y = 0; y < dimensions.y; ++y)
            for (int32_t x = 0; x < dimensions.x; ++x) {
                const Int3 sourcePosition{minimum_.x + x, minimum_.y + y, minimum_.z + z};
                if (source.contains(sourcePosition)) result.set({x, y, z}, source.get(sourcePosition));
            }
    return result;
}
VoxelDiff VoxelSelection::paste(const VoxelStructure& source, VoxelStructure& target, Int3 origin) const {
    std::vector<Change> changes;
    const Int3 size = source.size();
    for (int32_t z = 0; z < size.z; ++z)
        for (int32_t y = 0; y < size.y; ++y)
            for (int32_t x = 0; x < size.x; ++x) {
                const Int3 destination{origin.x + x, origin.y + y, origin.z + z};
                if (!target.contains(destination)) continue;
                const VoxelValue before = target.get(destination);
                const VoxelValue after = source.get({x, y, z});
                if (before != after)
                    changes.push_back({static_cast<uint32_t>(target.linear_index(destination)), {before, after}});
            }
    VoxelDiff diff = VoxelDiff::from_changes(std::move(changes));
    diff.redo(target);
    return diff;
}

BrushPreview VoxelTools::preview(const VoxelStructure& target, const VoxelBrushOperation& operation,
                                 std::optional<VoxelSelection> selection) {
    BrushPreview result;
    for (const Change& change : evaluate(target, operation, selection))
        result.voxels_.push_back({target.position_from_index(change.first), change.second.first, change.second.second});
    return result;
}
VoxelDiff VoxelTools::apply(VoxelStructure& target, const VoxelBrushOperation& operation,
                            std::optional<VoxelSelection> selection) {
    VoxelDiff diff = VoxelDiff::from_changes(evaluate(target, operation, selection));
    diff.redo(target);
    return diff;
}

VoxelDiff VoxelUndoStack::execute(VoxelStructure& target, const VoxelBrushOperation& operation,
                                  std::optional<VoxelSelection> selection) {
    VoxelDiff diff = VoxelTools::apply(target, operation, selection);
    if (diff.empty() || capacity_ == 0) return diff;
    if (undo_.size() == capacity_) undo_.erase(undo_.begin());
    undo_.push_back(diff);
    redo_.clear();
    return diff;
}
bool VoxelUndoStack::undo(VoxelStructure& target) {
    if (undo_.empty()) return false;
    VoxelDiff diff = std::move(undo_.back());
    undo_.pop_back();
    diff.undo(target);
    redo_.push_back(std::move(diff));
    return true;
}
bool VoxelUndoStack::redo(VoxelStructure& target) {
    if (redo_.empty()) return false;
    VoxelDiff diff = std::move(redo_.back());
    redo_.pop_back();
    diff.redo(target);
    undo_.push_back(std::move(diff));
    return true;
}
void VoxelUndoStack::clear() { undo_.clear(); redo_.clear(); }

} // namespace Engine::Voxel
