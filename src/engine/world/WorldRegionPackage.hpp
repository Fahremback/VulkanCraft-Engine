#pragma once

#include "WorldTypes.hpp"
#include <optional>
#include <span>
#include <unordered_map>

namespace Engine::World {

struct RegionPackageEntry {
    CellCoord coordinate;
    uint64_t offset{};
    uint64_t size{};
    uint64_t contentHash{};
};

struct RegionPackageManifest {
    uint32_t version{1};
    std::vector<RegionPackageEntry> cells;
};

class WorldRegionPackage final : public IWorldCellProvider {
public:
    explicit WorldRegionPackage(std::filesystem::path packagePath);
    [[nodiscard]] bool open(std::string& error);
    [[nodiscard]] const RegionPackageManifest& manifest() const noexcept { return manifest_; }
    [[nodiscard]] bool contains(CellCoord cell) const;
    [[nodiscard]] bool load(const CellDescriptor& descriptor, CellPayload& payload, std::string& error) override;
    void unload(const CellDescriptor&, CellPayload&&) override {}

    [[nodiscard]] static bool write(const std::filesystem::path& output,
                                    std::span<const std::pair<CellCoord, CellPayload>> cells,
                                    std::string& error);
private:
    std::filesystem::path path_;
    RegionPackageManifest manifest_;
    std::unordered_map<CellCoord, RegionPackageEntry, CellCoordHash> entries_;
};

class RegionPackager final {
public:
    void add(CellCoord coordinate, CellPayload payload);
    [[nodiscard]] bool remove(CellCoord coordinate);
    [[nodiscard]] bool package(const std::filesystem::path& output, std::string& error) const;
private:
    std::vector<std::pair<CellCoord, CellPayload>> cells_;
};

} // namespace Engine::World
