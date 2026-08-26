// NavStreaming.cpp — the only TU implementing the public nav streaming
// contract (Agente 4 §2 item 29 CORE): active-region gating over nav tiles.
// The active set is the square focus ± radius; load/unload ledgers track
// state; invalidation marks active tiles for rebuild (safe retry after
// loading). Pure std; deterministic order ((x, z) via std::map).

#include "engine/navigation/INavStreaming.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace engine {
namespace navigation {
namespace {

class NavStreaming final : public INavStreaming {
public:
    NavStreaming() = default;

    bool configure(std::size_t radiusTiles, std::string& errorOut) override {
        if (radiusTiles == 0) {
            errorOut = "nav streaming: radiusTiles must be >= 1";
            return false;
        }
        radius_ = radiusTiles;
        focus_ = { 0, 0 };
        loaded_.clear();
        invalid_.clear();
        return true;
    }

    void set_focus(std::int32_t tileX, std::int32_t tileZ) override {
        focus_ = { tileX, tileZ };
    }

    void focus(NavTile& out) const override { out = focus_; }

    bool is_tile_active(const NavTile& tile) const override {
        const long dx = static_cast<long>(tile.x) - focus_.x;
        const long dz = static_cast<long>(tile.z) - focus_.z;
        const long radius = static_cast<long>(radius_);
        return dx >= -radius && dx <= radius && dz >= -radius && dz <= radius;
    }

    std::vector<NavTile> tiles_to_load() const override {
        std::vector<NavTile> out;
        for (const NavTile& tile : active_tiles()) {
            if (loaded_.count(tile) == 0) out.push_back(tile);
        }
        return out;
    }

    std::vector<NavTile> tiles_to_unload() const override {
        std::vector<NavTile> out;
        for (const auto& entry : loaded_) {
            if (!is_tile_active(entry.first)) out.push_back(entry.first);
        }
        return out;
    }

    bool is_loaded(const NavTile& tile) const override {
        return loaded_.count(tile) != 0;
    }

    bool mark_loaded(const NavTile& tile) override {
        if (!is_tile_active(tile)) return false;
        loaded_[tile] = true;
        return true;
    }

    bool mark_unloaded(const NavTile& tile) override {
        const auto found = loaded_.find(tile);
        if (found == loaded_.end()) return false;
        loaded_.erase(found);
        invalid_.erase(tile);  // descarregar zera a marca de inválido
        return true;
    }

    bool invalidate_tile(const NavTile& tile) override {
        if (!is_tile_active(tile)) return false;
        invalid_[tile] = true;
        return true;
    }

    std::vector<NavTile> tiles_pending_rebuild() const override {
        std::vector<NavTile> out;
        for (const auto& entry : invalid_) {
            if (entry.second && is_tile_active(entry.first) &&
                loaded_.count(entry.first) != 0) {
                out.push_back(entry.first);
            }
        }
        return out;
    }

    std::size_t loaded_count() const override { return loaded_.size(); }
    void clear() override {
        loaded_.clear();
        invalid_.clear();
        focus_ = { 0, 0 };
    }

private:
    std::vector<NavTile> active_tiles() const {
        std::vector<NavTile> out;
        const long radius = static_cast<long>(radius_);
        for (long x = static_cast<long>(focus_.x) - radius;
             x <= static_cast<long>(focus_.x) + radius; ++x) {
            for (long z = static_cast<long>(focus_.z) - radius;
                 z <= static_cast<long>(focus_.z) + radius; ++z) {
                out.push_back({ static_cast<std::int32_t>(x),
                                static_cast<std::int32_t>(z) });
            }
        }
        return out;
    }

    std::size_t radius_{ 1 };
    NavTile focus_{ 0, 0 };
    std::map<NavTile, bool> loaded_;
    std::map<NavTile, bool> invalid_;
};

}  // namespace

std::unique_ptr<INavStreaming> create_nav_streaming() {
    return std::make_unique<NavStreaming>();
}

}  // namespace navigation
}  // namespace engine
