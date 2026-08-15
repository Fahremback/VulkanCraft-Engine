#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace Engine::World {

struct CellCoord {
    int32_t x{};
    int32_t z{};
    auto operator<=>(const CellCoord&) const = default;
};

struct CellCoordHash {
    size_t operator()(const CellCoord& value) const noexcept {
        const auto x = static_cast<uint32_t>(value.x);
        const auto z = static_cast<uint32_t>(value.z);
        return (static_cast<size_t>(x) << 32U) ^ static_cast<size_t>(z);
    }
};

enum class CellState : uint8_t { Unloaded, Loading, Loaded, Active, Unloading, Failed };

struct CellBounds {
    glm::vec3 minimum{0.0f};
    glm::vec3 maximum{0.0f};
    [[nodiscard]] bool contains(const glm::vec3& point) const noexcept;
    [[nodiscard]] float distance_squared(const glm::vec3& point) const noexcept;
};

struct CellDescriptor {
    CellCoord coordinate;
    CellBounds bounds;
    std::filesystem::path packagePath;
    uint64_t estimatedBytes{};
    bool alwaysLoaded{};
};

struct CellPayload {
    uint32_t formatVersion{1};
    std::vector<std::byte> bytes;
};

struct StreamingSource {
    uint64_t id{};
    std::string name;
    glm::vec3 position{0.0f};
    float loadRadius{512.0f};
    float activationRadius{256.0f};
    float unloadHysteresis{64.0f};
    int32_t priority{};
    bool enabled{true};
};

struct CellRuntimeSnapshot {
    CellCoord coordinate;
    CellState state{CellState::Unloaded};
    uint64_t residentBytes{};
    int32_t priority{};
    std::string lastError;
};

class IWorldCellProvider {
public:
    virtual ~IWorldCellProvider() = default;
    [[nodiscard]] virtual bool load(const CellDescriptor& descriptor, CellPayload& payload, std::string& error) = 0;
    virtual void unload(const CellDescriptor& descriptor, CellPayload&& payload) = 0;
};

} // namespace Engine::World
