#include "WorldRegionPackage.hpp"
#include <algorithm>
#include <array>
#include <fstream>

namespace Engine::World {
namespace {
constexpr std::array<char, 8> Magic{'V','C','R','E','G','I','O','N'};
struct Header { std::array<char, 8> magic; uint32_t version; uint32_t cellCount; };
uint64_t hash_bytes(std::span<const std::byte> bytes) {
    uint64_t hash = 1469598103934665603ULL;
    for (const std::byte value : bytes) { hash ^= static_cast<uint8_t>(value); hash *= 1099511628211ULL; }
    return hash;
}
}

WorldRegionPackage::WorldRegionPackage(std::filesystem::path packagePath) : path_(std::move(packagePath)) {}

bool WorldRegionPackage::open(std::string& error) {
    std::ifstream stream(path_, std::ios::binary);
    Header header{};
    if (!stream.read(reinterpret_cast<char*>(&header), sizeof(header)) || header.magic != Magic || header.version != 1) {
        error = "Invalid or unsupported world region package: " + path_.string(); return false;
    }
    manifest_ = {header.version, {}};
    entries_.clear();
    manifest_.cells.resize(header.cellCount);
    if (header.cellCount && !stream.read(reinterpret_cast<char*>(manifest_.cells.data()),
                                         static_cast<std::streamsize>(header.cellCount * sizeof(RegionPackageEntry)))) {
        error = "Truncated world region manifest: " + path_.string(); manifest_.cells.clear(); return false;
    }
    for (const auto& entry : manifest_.cells) entries_[entry.coordinate] = entry;
    error.clear();
    return true;
}

bool WorldRegionPackage::contains(CellCoord cell) const { return entries_.contains(cell); }

bool WorldRegionPackage::load(const CellDescriptor& descriptor, CellPayload& payload, std::string& error) {
    const auto it = entries_.find(descriptor.coordinate);
    if (it == entries_.end()) { error = "Cell is not present in region package"; return false; }
    std::ifstream stream(path_, std::ios::binary);
    payload = {};
    payload.bytes.resize(static_cast<size_t>(it->second.size));
    stream.seekg(static_cast<std::streamoff>(it->second.offset));
    if (it->second.size && !stream.read(reinterpret_cast<char*>(payload.bytes.data()), static_cast<std::streamsize>(it->second.size))) {
        error = "Truncated cell payload in region package"; payload.bytes.clear(); return false;
    }
    if (hash_bytes(payload.bytes) != it->second.contentHash) {
        error = "Cell payload checksum mismatch"; payload.bytes.clear(); return false;
    }
    error.clear();
    return true;
}

bool WorldRegionPackage::write(const std::filesystem::path& output,
        std::span<const std::pair<CellCoord, CellPayload>> cells, std::string& error) {
    std::vector<RegionPackageEntry> entries;
    entries.reserve(cells.size());
    uint64_t offset = sizeof(Header) + cells.size() * sizeof(RegionPackageEntry);
    for (const auto& [coordinate, payload] : cells) {
        entries.push_back({coordinate, offset, payload.bytes.size(), hash_bytes(payload.bytes)});
        offset += payload.bytes.size();
    }
    if (output.has_parent_path()) std::filesystem::create_directories(output.parent_path());
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    const Header header{Magic, 1, static_cast<uint32_t>(cells.size())};
    if (!stream.write(reinterpret_cast<const char*>(&header), sizeof(header))) { error = "Cannot create region package"; return false; }
    if (!entries.empty()) stream.write(reinterpret_cast<const char*>(entries.data()), static_cast<std::streamsize>(entries.size() * sizeof(RegionPackageEntry)));
    for (const auto& [_, payload] : cells)
        if (!payload.bytes.empty()) stream.write(reinterpret_cast<const char*>(payload.bytes.data()), static_cast<std::streamsize>(payload.bytes.size()));
    if (!stream) { error = "Failed while writing region package"; return false; }
    error.clear(); return true;
}

void RegionPackager::add(CellCoord coordinate, CellPayload payload) {
    if (auto it = std::find_if(cells_.begin(), cells_.end(), [coordinate](const auto& c) { return c.first == coordinate; }); it != cells_.end())
        it->second = std::move(payload);
    else cells_.emplace_back(coordinate, std::move(payload));
}

bool RegionPackager::remove(CellCoord coordinate) {
    return std::erase_if(cells_, [coordinate](const auto& c) { return c.first == coordinate; }) != 0;
}

bool RegionPackager::package(const std::filesystem::path& output, std::string& error) const {
    auto sorted = cells_;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return WorldRegionPackage::write(output, sorted, error);
}

} // namespace Engine::World
