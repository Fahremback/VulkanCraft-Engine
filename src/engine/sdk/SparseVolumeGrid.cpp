// SparseVolumeGrid.cpp — adapter for ISparseVolumeGrid.
// OpenVDB-style sparse volume: 8x8x8 brick tree, leaf-centric.
// Headless, deterministic, no GPU required.

#include "engine/rendering/ISparseVolumeGrid.hpp"
#include <cstring>
#include <cmath>
#include <sstream>
#include <unordered_map>

namespace vc::rendering {

// ─── Config ───────────────────────────────────────

bool SparseVolumeConfig::validate() const {
    if (bricksX < 1 || bricksY < 1 || bricksZ < 1) return false;
    if (fogMaxDist < 0.0f) return false;
    if (fogDecay < 0.0f) return false;
    return true;
}

static std::string je2(const std::string& s) {
    std::string o;
    for (char c : s) { if(c=='"')o+="\\\""; else if(c=='\\')o+="\\\\"; else o+=c; }
    return o;
}

std::string SparseVolumeConfig::toJson() const {
    std::ostringstream o;
    o << "{\"bricksX\":" << bricksX << ",\"bricksY\":" << bricksY
      << ",\"bricksZ\":" << bricksZ << ",\"fogMaxDist\":" << fogMaxDist
      << ",\"fogDecay\":" << fogDecay << ",\"sdfThreshold\":" << sdfThreshold << "}";
    return o.str();
}

static float jsonFloat(const std::string& j, const std::string& k, float def) {
    auto n = "\"" + k + "\""; auto p = j.find(n);
    if (p == std::string::npos) return def;
    p = j.find(':', p + n.size()); if (p == std::string::npos) return def;
    return std::strtof(j.c_str() + p + 1, nullptr);
}

static int jsonInt(const std::string& j, const std::string& k, int def) {
    return static_cast<int>(jsonFloat(j, k, static_cast<float>(def)));
}

SparseVolumeConfig SparseVolumeConfig::fromJson(const std::string& j, std::string& err) {
    SparseVolumeConfig c;
    c.bricksX = jsonInt(j, "bricksX", 16);
    c.bricksY = jsonInt(j, "bricksY", 16);
    c.bricksZ = jsonInt(j, "bricksZ", 16);
    c.fogMaxDist = jsonFloat(j, "fogMaxDist", 8.0f);
    c.fogDecay = jsonFloat(j, "fogDecay", 2.0f);
    c.sdfThreshold = jsonFloat(j, "sdfThreshold", 0.0f);
    if (!c.validate()) { err = "invalid config"; return {}; }
    return c;
}

// ─── Adapter ──────────────────────────────────────

struct BrickKey {
    int x, y, z;
    bool operator==(const BrickKey& o) const { return x==o.x && y==o.y && z==o.z; }
};

struct BrickHash {
    size_t operator()(const BrickKey& k) const {
        return std::hash<int>()(k.x) ^ (std::hash<int>()(k.y) << 16) ^ (std::hash<int>()(k.z) << 24);
    }
};

class SparseVolumeGridImpl : public ISparseVolumeGrid {
public:
    SparseVolumeGridImpl(const SparseVolumeConfig& cfg) : config_(cfg) {}

    void setVoxel(int x, int y, int z, float value) override {
        int bx = x / VolumeBrick::SIZE, by = y / VolumeBrick::SIZE, bz = z / VolumeBrick::SIZE;
        auto& brick = getOrCreateBrick(bx, by, bz);
        int lx = x - bx * VolumeBrick::SIZE;
        int ly = y - by * VolumeBrick::SIZE;
        int lz = z - bz * VolumeBrick::SIZE;
        brick.voxels[lx + ly * VolumeBrick::SIZE + lz * VolumeBrick::SIZE * VolumeBrick::SIZE] = value;
    }

    float getVoxel(int x, int y, int z) const override {
        int bx = x / VolumeBrick::SIZE, by = y / VolumeBrick::SIZE, bz = z / VolumeBrick::SIZE;
        auto it = bricks_.find({bx, by, bz});
        if (it == bricks_.end()) return 0.0f;
        int lx = x - bx * VolumeBrick::SIZE;
        int ly = y - by * VolumeBrick::SIZE;
        int lz = z - bz * VolumeBrick::SIZE;
        return it->second.voxels[lx + ly * VolumeBrick::SIZE + lz * VolumeBrick::SIZE * VolumeBrick::SIZE];
    }

    float sample(float fx, float fy, float fz) const override {
        // Trilinear interpolation.
        int x0 = static_cast<int>(std::floor(fx));
        int y0 = static_cast<int>(std::floor(fy));
        int z0 = static_cast<int>(std::floor(fz));
        float tx = fx - x0, ty = fy - y0, tz = fz - z0;
        float c000 = getVoxel(x0,   y0,   z0);
        float c100 = getVoxel(x0+1, y0,   z0);
        float c010 = getVoxel(x0,   y0+1, z0);
        float c110 = getVoxel(x0+1, y0+1, z0);
        float c001 = getVoxel(x0,   y0,   z0+1);
        float c101 = getVoxel(x0+1, y0,   z0+1);
        float c011 = getVoxel(x0,   y0+1, z0+1);
        float c111 = getVoxel(x0+1, y0+1, z0+1);
        float c00 = c000*(1-tx) + c100*tx;
        float c01 = c001*(1-tx) + c101*tx;
        float c10 = c010*(1-tx) + c110*tx;
        float c11 = c011*(1-tx) + c111*tx;
        float c0 = c00*(1-ty) + c10*ty;
        float c1 = c01*(1-ty) + c11*ty;
        return c0*(1-tz) + c1*tz;
    }

    float sdf(float x, float y, float z) const override {
        // Trilinear SDF gradient via central differences.
        float eps = 0.5f;
        float dx = sample(x+eps,y,z) - sample(x-eps,y,z);
        float dy = sample(x,y+eps,z) - sample(x,y-eps,z);
        float dz = sample(x,y,z+eps) - sample(x,y,z-eps);
        float len = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (len < 1e-8f) return 0.0f;
        // SDF = distance to zero-crossing.
        float val = sample(x, y, z);
        return val / len;
    }

    float fogOfWar(int x, int y, int z,
                   float cx, float cy, float cz) const override {
        float dx = x - cx, dy = y - cy, dz = z - cz;
        float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (dist >= config_.fogMaxDist) return 0.0f;
        float t = 1.0f - dist / config_.fogMaxDist;
        return std::pow(t, config_.fogDecay);
    }

    void floodFill(int cx, int cy, int cz, int radius) override {
        for (int dz = -radius; dz <= radius; dz++)
            for (int dy = -radius; dy <= radius; dy++)
                for (int dx = -radius; dx <= radius; dx++)
                    if (dx*dx + dy*dy + dz*dz <= radius*radius)
                        setVoxel(cx+dx, cy+dy, cz+dz, 1.0f);
    }

    int activeBrickCount() const override {
        return static_cast<int>(bricks_.size());
    }

    const VolumeBrick* getBrick(int bx, int by, int bz) const override {
        auto it = bricks_.find({bx, by, bz});
        return it != bricks_.end() ? &it->second : nullptr;
    }

    std::vector<uint32_t> serialize() const override {
        std::vector<uint32_t> data;
        // Header: magic, brick count, config.
        data.push_back(0x53504152); // "SPAR"
        data.push_back(static_cast<uint32_t>(bricks_.size()));
        data.push_back(static_cast<uint32_t>(config_.bricksX));
        data.push_back(static_cast<uint32_t>(config_.bricksY));
        data.push_back(static_cast<uint32_t>(config_.bricksZ));
        // Bricks: key (3 ints) + 512 floats.
        for (auto& [key, brick] : bricks_) {
            data.push_back(static_cast<uint32_t>(key.x));
            data.push_back(static_cast<uint32_t>(key.y));
            data.push_back(static_cast<uint32_t>(key.z));
            const uint32_t* raw = reinterpret_cast<const uint32_t*>(brick.voxels);
            data.insert(data.end(), raw, raw + VolumeBrick::SIZE * VolumeBrick::SIZE * VolumeBrick::SIZE);
        }
        return data;
    }

    bool deserialize(const uint32_t* data, size_t wordCount, std::string& err) override {
        if (wordCount < 5) { err = "too short"; return false; }
        if (data[0] != 0x53504152) { err = "bad magic"; return false; }
        uint32_t brickCount = data[1];
        size_t expected = 5 + brickCount * (3 + 512);
        if (wordCount < expected) { err = "truncated"; return false; }
        bricks_.clear();
        size_t pos = 5;
        for (uint32_t i = 0; i < brickCount; i++) {
            BrickKey key;
            key.x = static_cast<int>(data[pos]);
            key.y = static_cast<int>(data[pos+1]);
            key.z = static_cast<int>(data[pos+2]);
            pos += 3;
            VolumeBrick brick;
            std::memcpy(brick.voxels, data + pos, sizeof(brick.voxels));
            pos += VolumeBrick::SIZE * VolumeBrick::SIZE * VolumeBrick::SIZE;
            bricks_[key] = brick;
        }
        return true;
    }

private:
    SparseVolumeConfig config_;
    std::unordered_map<BrickKey, VolumeBrick, BrickHash> bricks_;

    VolumeBrick& getOrCreateBrick(int bx, int by, int bz) {
        return bricks_[{bx, by, bz}];
    }
};

std::unique_ptr<ISparseVolumeGrid> create_sparse_volume_grid(
    const SparseVolumeConfig& config, std::string& errorOut) {
    if (!config.validate()) { errorOut = "invalid config"; return nullptr; }
    return std::make_unique<SparseVolumeGridImpl>(config);
}

} // namespace vc::rendering
