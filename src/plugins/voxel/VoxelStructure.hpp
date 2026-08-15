#pragma once

#include "../../engine/assets/AssetRegistry.hpp"

#include <cstddef>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Engine::Voxel {

struct Int3 {
    int32_t x{};
    int32_t y{};
    int32_t z{};
    auto operator<=>(const Int3&) const = default;
};

struct VoxelValue {
    uint16_t type{};
    uint16_t material{};
    uint8_t density{};

    [[nodiscard]] bool empty() const noexcept { return type == 0 || density == 0; }
    static constexpr VoxelValue air() noexcept { return {}; }
    auto operator<=>(const VoxelValue&) const = default;
};

struct VoxelSocket {
    std::string name;
    Int3 position;
    bool operator==(const VoxelSocket&) const = default;
};

struct VoxelEntityReference {
    std::string assetReference;
    Int3 position;
    bool operator==(const VoxelEntityReference&) const = default;
};

// Generic, engine-owned voxel asset. Game block catalogs and authored structures
// stay in project content and are represented only by stable numeric/string IDs.
class VoxelStructure final {
public:
    VoxelStructure() = default;
    explicit VoxelStructure(Int3 size, std::string name = {});

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void set_name(std::string value) { name_ = std::move(value); }
    [[nodiscard]] Int3 size() const noexcept { return size_; }
    [[nodiscard]] Int3 pivot() const noexcept { return pivot_; }
    void set_pivot(Int3 pivot);
    [[nodiscard]] size_t voxel_count() const noexcept { return voxels_.size(); }
    [[nodiscard]] bool contains(Int3 position) const noexcept;
    [[nodiscard]] size_t linear_index(Int3 position) const;
    [[nodiscard]] Int3 position_from_index(size_t index) const;
    [[nodiscard]] VoxelValue get(Int3 position) const;
    bool set(Int3 position, VoxelValue value);
    [[nodiscard]] const std::vector<VoxelValue>& voxels() const noexcept { return voxels_; }

    void add_socket(VoxelSocket socket);
    void set_variant(std::string key, std::string assetReference);
    void add_entity(VoxelEntityReference entity);
    [[nodiscard]] const std::vector<VoxelSocket>& sockets() const noexcept { return sockets_; }
    [[nodiscard]] const std::unordered_map<std::string, std::string>& variants() const noexcept { return variants_; }
    [[nodiscard]] const std::vector<VoxelEntityReference>& entities() const noexcept { return entities_; }

    bool operator==(const VoxelStructure&) const = default;

private:
    friend class VoxelStructureIO;
    Int3 size_{};
    Int3 pivot_{};
    std::string name_;
    std::vector<VoxelValue> voxels_;
    std::vector<VoxelSocket> sockets_;
    std::unordered_map<std::string, std::string> variants_;
    std::vector<VoxelEntityReference> entities_;
};

class VoxelStructureIO final {
public:
    static bool export_source(const VoxelStructure& structure, const std::filesystem::path& path,
                              std::string* error = nullptr);
    static bool import_source(const std::filesystem::path& path, VoxelStructure& structure,
                              std::string* error = nullptr);
    static bool cook(const VoxelStructure& structure, const std::filesystem::path& path,
                     std::string* error = nullptr);
    static bool load_cooked(const std::filesystem::path& path, VoxelStructure& structure,
                            std::string* error = nullptr);
};

class VoxelStructureImporter final : public AssetImporter {
public:
    static constexpr uint32_t version() noexcept { return 1; }
    [[nodiscard]] bool supports_extension(std::string_view extension) const override;
    [[nodiscard]] ImportResult import(const ImportRequest& request) const override;
};

} // namespace Engine::Voxel
