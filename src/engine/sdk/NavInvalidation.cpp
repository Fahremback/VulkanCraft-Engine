// NavInvalidation.cpp — the only TU implementing the public nav invalidation
// contract (Agente 4 §2 item 24 CORE): region → intersecting navmesh tiles,
// marked invalid with a monotonically increasing version. Pure std;
// tiles always returned in (x, z) order (std::map iteration).

#include "engine/navigation/INavInvalidation.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace engine {
namespace navigation {
namespace {

bool finite_float(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float is 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

bool valid_region(const NavInvalidationRegion& region) {
    return finite_float(region.minX) && finite_float(region.minZ) &&
           finite_float(region.maxX) && finite_float(region.maxZ) &&
           region.minX <= region.maxX && region.minZ <= region.maxZ;
}

long floor_div(float value, float tileSize) {
    return static_cast<long>(std::floor(static_cast<double>(value) / tileSize));
}

class NavInvalidation final : public INavInvalidation {
public:
    NavInvalidation() = default;

    bool configure(float tileSize, std::string& errorOut) override {
        if (!finite_float(tileSize) || tileSize <= 0.0f) {
            errorOut = "nav invalidation: tileSize must be finite and > 0";
            return false;
        }
        tileSize_ = tileSize;
        tiles_.clear();
        version_ = 0;
        return true;
    }

    std::vector<NavTile> tiles_for(
        const NavInvalidationRegion& region) const override {
        std::vector<NavTile> out;
        if (!valid_region(region)) return out;
        const long minX = floor_div(region.minX, tileSize_);
        const long maxX = floor_div(region.maxX, tileSize_);
        const long minZ = floor_div(region.minZ, tileSize_);
        const long maxZ = floor_div(region.maxZ, tileSize_);
        for (long x = minX; x <= maxX; ++x) {
            for (long z = minZ; z <= maxZ; ++z) {
                out.push_back({ static_cast<std::int32_t>(x),
                                static_cast<std::int32_t>(z) });
            }
        }
        return out;  // já em ordem (x,z) pela varredura
    }

    void invalidate(const NavInvalidationRegion& region) override {
        if (!valid_region(region)) return;
        ++version_;
        for (const NavTile& tile : tiles_for(region)) {
            TileState& state = tiles_[tile];
            state.invalid = true;
            state.lastVersion = version_;
        }
    }

    bool is_invalid(const NavTile& tile) const override {
        const auto found = tiles_.find(tile);
        return found != tiles_.end() && found->second.invalid;
    }

    bool rebuild(const NavTile& tile) override {
        const auto found = tiles_.find(tile);
        if (found == tiles_.end() || !found->second.invalid) return false;
        found->second.invalid = false;
        return true;
    }

    std::vector<NavTile> invalid_tiles() const override {
        std::vector<NavTile> out;
        for (const auto& entry : tiles_) {
            if (entry.second.invalid) out.push_back(entry.first);
        }
        return out;  // map: ordem (x,z)
    }

    std::uint64_t version() const override { return version_; }

    std::vector<NavTile> invalidated_since(std::uint64_t version) const override {
        std::vector<NavTile> out;
        for (const auto& entry : tiles_) {
            if (entry.second.invalid && entry.second.lastVersion > version) {
                out.push_back(entry.first);
            }
        }
        return out;
    }

    void clear() override {
        tiles_.clear();
        version_ = 0;
    }

private:
    struct TileState {
        bool invalid{ false };
        std::uint64_t lastVersion{ 0 };
    };

    float tileSize_{ 1.0f };
    std::uint64_t version_{ 0 };
    std::map<NavTile, TileState> tiles_;
};

}  // namespace

std::unique_ptr<INavInvalidation> create_nav_invalidation() {
    return std::make_unique<NavInvalidation>();
}

}  // namespace navigation
}  // namespace engine
