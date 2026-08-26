// SpatialIndex.cpp — the only TU implementing the public spatial index
// contract (Agente 4 §1 item 15 CORE): a uniform grid over entity AABBs.
// Each entity is stamped into every cell its AABB overlaps; queries collect
// candidates from the overlapped cells, dedup and sort by id (deterministic).

#include "engine/entity/ISpatialIndex.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace engine {
namespace entity {
namespace {

bool finite_float(float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float is 32-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

bool valid_bounds(const SpatialBounds& bounds) {
    return finite_float(bounds.min.x) && finite_float(bounds.min.y) &&
           finite_float(bounds.min.z) && finite_float(bounds.max.x) &&
           finite_float(bounds.max.y) && finite_float(bounds.max.z) &&
           bounds.min.x <= bounds.max.x && bounds.min.y <= bounds.max.y &&
           bounds.min.z <= bounds.max.z;
}

struct CellKey {
    long x;
    long z;
    bool operator<(const CellKey& other) const {
        if (x != other.x) return x < other.x;
        return z < other.z;
    }
};

// Nível do chão da divisão truncada (floor para negativos).
long floor_div(float value, float cellSize) {
    const double v = static_cast<double>(value) / cellSize;
    return static_cast<long>(std::floor(v));
}

class SpatialIndex final : public ISpatialIndex {
public:
    SpatialIndex() = default;

    bool configure(float cellSize, std::string& errorOut) override {
        if (!finite_float(cellSize) || cellSize <= 0.0f) {
            errorOut = "spatial index: cellSize must be finite and > 0";
            return false;
        }
        cellSize_ = cellSize;
        cells_.clear();
        entities_.clear();
        return true;
    }

    bool insert(std::uint64_t entityId, const SpatialBounds& bounds,
                std::string& errorOut) override {
        if (entities_.count(entityId) != 0) {
            errorOut = "spatial index: duplicate entity id";
            return false;
        }
        if (!valid_bounds(bounds)) {
            errorOut = "spatial index: invalid bounds (non-finite or min > max)";
            return false;
        }
        stamp(entityId, bounds);
        entities_[entityId] = bounds;
        return true;
    }

    bool remove(std::uint64_t entityId) override {
        const auto found = entities_.find(entityId);
        if (found == entities_.end()) return false;
        unstamp(entityId, found->second);
        entities_.erase(found);
        return true;
    }

    bool move(std::uint64_t entityId, const SpatialBounds& newBounds) override {
        const auto found = entities_.find(entityId);
        if (found == entities_.end()) return false;
        if (!valid_bounds(newBounds)) return false;
        unstamp(entityId, found->second);
        stamp(entityId, newBounds);
        found->second = newBounds;
        return true;
    }

    std::vector<std::uint64_t> query_aabb(
        const SpatialBounds& query) const override {
        if (!valid_bounds(query)) return {};
        std::set<std::uint64_t> collected;
        const CellKey minCell = { floor_div(query.min.x, cellSize_),
                                  floor_div(query.min.z, cellSize_) };
        const CellKey maxCell = { floor_div(query.max.x, cellSize_),
                                  floor_div(query.max.z, cellSize_) };
        for (long cx = minCell.x; cx <= maxCell.x; ++cx) {
            for (long cz = minCell.z; cz <= maxCell.z; ++cz) {
                const auto cell = cells_.find({ cx, cz });
                if (cell == cells_.end()) continue;
                collected.insert(cell->second.begin(), cell->second.end());
            }
        }
        // Filtra pelo teste exato de interseção AABB (as células dão candidatos).
        std::vector<std::uint64_t> out;
        for (const std::uint64_t id : collected) {
            const SpatialBounds& bounds = entities_.at(id);
            if (bounds.max.x >= query.min.x && bounds.min.x <= query.max.x &&
                bounds.max.y >= query.min.y && bounds.min.y <= query.max.y &&
                bounds.max.z >= query.min.z && bounds.min.z <= query.max.z) {
                out.push_back(id);
            }
        }
        return out;  // std::set já ordena por id
    }

    std::vector<std::uint64_t> query_point(float x, float y,
                                           float z) const override {
        if (!finite_float(x) || !finite_float(y) || !finite_float(z)) return {};
        const CellKey cell = { floor_div(x, cellSize_), floor_div(z, cellSize_) };
        const auto found = cells_.find(cell);
        if (found == cells_.end()) return {};
        std::vector<std::uint64_t> out;
        for (const std::uint64_t id : found->second) {  // set: ordem crescente
            const SpatialBounds& bounds = entities_.at(id);
            if (x >= bounds.min.x && x <= bounds.max.x &&
                y >= bounds.min.y && y <= bounds.max.y &&
                z >= bounds.min.z && z <= bounds.max.z) {
                out.push_back(id);
            }
        }
        return out;
    }

    std::size_t count() const override { return entities_.size(); }
    void clear() override {
        cells_.clear();
        entities_.clear();
    }

private:
    void stamp(std::uint64_t entityId, const SpatialBounds& bounds) {
        const CellKey minCell = { floor_div(bounds.min.x, cellSize_),
                                  floor_div(bounds.min.z, cellSize_) };
        const CellKey maxCell = { floor_div(bounds.max.x, cellSize_),
                                  floor_div(bounds.max.z, cellSize_) };
        for (long cx = minCell.x; cx <= maxCell.x; ++cx) {
            for (long cz = minCell.z; cz <= maxCell.z; ++cz) {
                cells_[{ cx, cz }].insert(entityId);
            }
        }
    }

    void unstamp(std::uint64_t entityId, const SpatialBounds& bounds) {
        const CellKey minCell = { floor_div(bounds.min.x, cellSize_),
                                  floor_div(bounds.min.z, cellSize_) };
        const CellKey maxCell = { floor_div(bounds.max.x, cellSize_),
                                  floor_div(bounds.max.z, cellSize_) };
        for (long cx = minCell.x; cx <= maxCell.x; ++cx) {
            for (long cz = minCell.z; cz <= maxCell.z; ++cz) {
                const auto cell = cells_.find({ cx, cz });
                if (cell == cells_.end()) continue;
                cell->second.erase(entityId);
                if (cell->second.empty()) cells_.erase(cell);
            }
        }
    }

    float cellSize_{ 1.0f };
    std::map<CellKey, std::set<std::uint64_t>> cells_;
    std::map<std::uint64_t, SpatialBounds> entities_;
};

}  // namespace

std::unique_ptr<ISpatialIndex> create_spatial_index() {
    return std::make_unique<SpatialIndex>();
}

}  // namespace entity
}  // namespace engine
