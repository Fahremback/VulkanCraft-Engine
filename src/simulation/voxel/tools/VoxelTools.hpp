#pragma once

#include "../../../plugins/voxel/VoxelStructure.hpp"

#include <glm/glm.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Engine {

enum class VoxelBrushShape { Sphere, Box, Cube = Box };
enum class VoxelBrushMode { Add, Remove, Replace, Smooth, Flatten, Paint };

struct VoxelBrushOperation {
    VoxelBrushShape shape{VoxelBrushShape::Sphere};
    VoxelBrushMode mode{VoxelBrushMode::Add};
    glm::vec3 position{0.0f};
    glm::vec3 halfExtents{1.0f};
    float radius{3.0f};
    uint16_t voxelType{1};
    uint16_t material{};
    uint8_t density{255};
    int32_t flattenHeight{};
};

} // namespace Engine

namespace Engine::Voxel {

class VoxelDiff final {
public:
    struct Run {
        uint32_t firstIndex{};
        std::vector<VoxelValue> before;
        std::vector<VoxelValue> after;
    };

    [[nodiscard]] bool empty() const noexcept { return changed_voxels() == 0; }
    [[nodiscard]] size_t changed_voxels() const noexcept;
    [[nodiscard]] size_t storage_bytes() const noexcept;
    [[nodiscard]] const std::vector<Run>& runs() const noexcept { return runs_; }
    void undo(VoxelStructure& target) const;
    void redo(VoxelStructure& target) const;

private:
    friend class VoxelTools;
    friend class VoxelSelection;
    static VoxelDiff from_changes(std::vector<std::pair<uint32_t, std::pair<VoxelValue, VoxelValue>>> changes);
    std::vector<Run> runs_;
};

struct PreviewVoxel {
    Int3 position;
    VoxelValue current;
    VoxelValue proposed;
};

class BrushPreview final {
public:
    [[nodiscard]] bool empty() const noexcept { return voxels_.empty(); }
    [[nodiscard]] size_t changed_voxels() const noexcept { return voxels_.size(); }
    [[nodiscard]] const std::vector<PreviewVoxel>& voxels() const noexcept { return voxels_; }
private:
    friend class VoxelTools;
    std::vector<PreviewVoxel> voxels_;
};

class VoxelSelection final {
public:
    VoxelSelection(Int3 first, Int3 second);
    [[nodiscard]] Int3 minimum() const noexcept { return minimum_; }
    [[nodiscard]] Int3 maximum() const noexcept { return maximum_; }
    [[nodiscard]] bool contains(Int3 position) const noexcept;
    [[nodiscard]] VoxelStructure extract(const VoxelStructure& source, std::string name = {}) const;
    [[nodiscard]] VoxelDiff paste(const VoxelStructure& source, VoxelStructure& target, Int3 origin) const;
private:
    Int3 minimum_{};
    Int3 maximum_{};
};

class VoxelTools final {
public:
    [[nodiscard]] static BrushPreview preview(const VoxelStructure& target,
                                              const VoxelBrushOperation& operation,
                                              std::optional<VoxelSelection> selection = std::nullopt);
    [[nodiscard]] static VoxelDiff apply(VoxelStructure& target,
                                         const VoxelBrushOperation& operation,
                                         std::optional<VoxelSelection> selection = std::nullopt);
};

class VoxelUndoStack final {
public:
    explicit VoxelUndoStack(size_t capacity = 128) : capacity_(capacity) {}
    [[nodiscard]] VoxelDiff execute(VoxelStructure& target, const VoxelBrushOperation& operation,
                                    std::optional<VoxelSelection> selection = std::nullopt);
    bool undo(VoxelStructure& target);
    bool redo(VoxelStructure& target);
    void clear();
    [[nodiscard]] size_t undo_count() const noexcept { return undo_.size(); }
    [[nodiscard]] size_t redo_count() const noexcept { return redo_.size(); }
private:
    size_t capacity_;
    std::vector<VoxelDiff> undo_;
    std::vector<VoxelDiff> redo_;
};

} // namespace Engine::Voxel
