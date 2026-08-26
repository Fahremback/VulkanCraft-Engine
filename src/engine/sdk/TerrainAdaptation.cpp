// TerrainAdaptation.cpp — adapter único de ITerrainAdaptation.
// Heightmap em grade (amostragem bilinear com clamp) + alinhamento de pés:
// d = min_f(H_f + off_f − (R + oy_f)); root_y' = R + d; cada pé recebe o
// delta restante. JSON bit-exact all-or-nothing.

#include "engine/animation/ITerrainAdaptation.hpp"

#include "engine/sdk/RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <utility>

namespace engine::animation {
namespace {

std::string json_escape(const std::string& text) {
    std::ostringstream out;
    for (char c : text) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out << "\\u" << std::hex << std::setw(4)
                        << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(c))
                        << std::dec;
                } else {
                    out << c;
                }
        }
    }
    return out.str();
}

struct Heightmap {
    double origin_x = 0.0;
    double origin_z = 0.0;
    double cell = 1.0;
    int cols = 0;
    int rows = 0;
    std::vector<double> heights;  // row-major, cols*rows
};

struct Config {
    std::string root_bone;
    std::vector<FootConfig> feet;
};

class TerrainAdaptation final : public ITerrainAdaptation {
public:
    bool set_heightmap(const std::string& terrainId, double origin_x,
                       double origin_z, double cell_size,
                       const std::vector<double>& heights, int cols, int rows,
                       std::string& errorOut) override {
        if (terrainId.empty()) {
            errorOut = "terrain id must not be empty";
            return false;
        }
        if (!std::isfinite(cell_size) || cell_size <= 0.0) {
            errorOut = "cell size must be finite and > 0";
            return false;
        }
        if (cols <= 0 || rows <= 0) {
            errorOut = "grid must have positive dimensions";
            return false;
        }
        const std::size_t expected = static_cast<std::size_t>(cols) *
                                     static_cast<std::size_t>(rows);
        if (heights.size() != expected) {
            errorOut = "heights size must equal cols*rows";
            return false;
        }
        for (double h : heights) {
            if (!std::isfinite(h)) {
                errorOut = "heights must be finite";
                return false;
            }
        }
        Heightmap m;
        m.origin_x = origin_x;
        m.origin_z = origin_z;
        m.cell = cell_size;
        m.cols = cols;
        m.rows = rows;
        m.heights = heights;
        maps_[terrainId] = std::move(m);
        errorOut.clear();
        return true;
    }

    double height_at(const std::string& terrainId, double x, double z,
                     std::string& errorOut) const override {
        const auto it = maps_.find(terrainId);
        if (it == maps_.end()) {
            errorOut = "unknown terrain \"" + terrainId + "\"";
            return 0.0;
        }
        const Heightmap& m = it->second;
        if (!std::isfinite(x) || !std::isfinite(z)) {
            errorOut = "sample position must be finite";
            return 0.0;
        }
        // Coordenada de célula (clamp nas bordas).
        const double fx = (x - m.origin_x) / m.cell;
        const double fz = (z - m.origin_z) / m.cell;
        const double cx = std::max(0.0, std::min(fx, static_cast<double>(m.cols - 1)));
        const double cz = std::max(0.0, std::min(fz, static_cast<double>(m.rows - 1)));
        const int i0 = std::min(static_cast<int>(cx), m.cols - 1);
        const int j0 = std::min(static_cast<int>(cz), m.rows - 1);
        const int i1 = std::min(i0 + 1, m.cols - 1);
        const int j1 = std::min(j0 + 1, m.rows - 1);
        const double tx = cx - static_cast<double>(i0);
        const double tz = cz - static_cast<double>(j0);
        const double h00 = m.heights[static_cast<std::size_t>(j0) * m.cols + i0];
        const double h10 = m.heights[static_cast<std::size_t>(j0) * m.cols + i1];
        const double h01 = m.heights[static_cast<std::size_t>(j1) * m.cols + i0];
        const double h11 = m.heights[static_cast<std::size_t>(j1) * m.cols + i1];
        const double top = h00 + (h10 - h00) * tx;
        const double bot = h01 + (h11 - h01) * tx;
        errorOut.clear();
        return top + (bot - top) * tz;
    }

    bool configure(const std::string& rootBone,
                   const std::vector<FootConfig>& feet,
                   std::string& errorOut) override {
        if (rootBone.empty()) {
            errorOut = "root bone must not be empty";
            return false;
        }
        std::map<std::string, bool> seen;
        for (const FootConfig& f : feet) {
            if (f.bone.empty()) {
                errorOut = "foot bone must not be empty";
                return false;
            }
            if (seen.count(f.bone) != 0) {
                errorOut = "duplicate foot bone \"" + f.bone + "\"";
                return false;
            }
            if (!std::isfinite(f.ground_offset)) {
                errorOut = "ground offset must be finite";
                return false;
            }
            seen[f.bone] = true;
        }
        Config cfg;
        cfg.root_bone = rootBone;
        cfg.feet = feet;
        config_ = std::move(cfg);
        errorOut.clear();
        return true;
    }

    TerrainAdaptationResult adapt(const std::string& terrainId, double root_x,
                                  double root_y, double root_z,
                                  std::string& errorOut) const override {
        TerrainAdaptationResult out;
        if (!std::isfinite(root_x) || !std::isfinite(root_y) ||
            !std::isfinite(root_z)) {
            errorOut = "root position must be finite";
            return out;
        }
        if (config_.root_bone.empty()) {
            errorOut = "no configuration set (call configure first)";
            return out;
        }
        const auto it = maps_.find(terrainId);
        if (it == maps_.end()) {
            errorOut = "unknown terrain \"" + terrainId + "\"";
            return out;
        }
        (void)it;
        // d = min_f(H_f + off_f − (R + oy_f)).
        double d = 0.0;
        std::vector<FootGroundResult> feet;
        for (const FootConfig& f : config_.feet) {
            const double hx = root_x + f.local_offset.x;
            const double hz = root_z + f.local_offset.z;
            const double ground =
                height_at(terrainId, hx, hz, errorOut);
            if (!errorOut.empty()) return out;
            const double target = ground + f.ground_offset;
            const double cur = root_y + f.local_offset.y;
            const double delta = target - cur;
            d = std::min(d, delta);
            FootGroundResult r;
            r.bone = f.bone;
            r.target_world_y = target;
            r.delta_y = delta;
            feet.push_back(r);
        }
        out.root_y = root_y + d;
        for (std::size_t i = 0; i < feet.size(); ++i) {
            // Delta restante após o root descer/sober d.
            feet[i].delta_y = feet[i].delta_y - d;
        }
        out.feet = std::move(feet);
        errorOut.clear();
        return out;
    }

    std::string serialize_state() const override {
        std::ostringstream out;
        out << std::setprecision(9);
        out << "{\"terrain\":{";
        bool first = true;
        for (const auto& kv : maps_) {
            if (!first) out << ",";
            first = false;
            const Heightmap& m = kv.second;
            out << "\"" << json_escape(kv.first) << "\":{\"origin_x\":"
                << m.origin_x << ",\"origin_z\":" << m.origin_z
                << ",\"cell\":" << m.cell << ",\"cols\":" << m.cols
                << ",\"rows\":" << m.rows << ",\"heights\":[";
            for (std::size_t i = 0; i < m.heights.size(); ++i) {
                if (i > 0) out << ",";
                out << m.heights[i];
            }
            out << "]}";
        }
        out << "},\"config\":{";
        if (!config_.root_bone.empty()) {
            out << "\"root\":\"" << json_escape(config_.root_bone)
                << "\",\"feet\":[";
            for (std::size_t i = 0; i < config_.feet.size(); ++i) {
                if (i > 0) out << ",";
                const FootConfig& f = config_.feet[i];
                out << "{\"bone\":\"" << json_escape(f.bone)
                    << "\",\"ox\":" << f.local_offset.x
                    << ",\"oy\":" << f.local_offset.y
                    << ",\"oz\":" << f.local_offset.z
                    << ",\"ground_offset\":" << f.ground_offset << "}";
            }
            out << "]";
        }
        out << "}}";
        return out.str();
    }

    bool deserialize_state(const std::string& json,
                           std::string& errorOut) override {
        sdk::JsonValue doc;
        if (!sdk::json_parse(json, doc, errorOut)) return false;
        if (!doc.is_object()) {
            errorOut = "terrain adaptation state must be an object";
            return false;
        }
        const sdk::JsonValue* terrain = doc.field("terrain");
        const sdk::JsonValue* config = doc.field("config");
        if (terrain == nullptr || !terrain->is_object() ||
            config == nullptr || !config->is_object()) {
            errorOut = "state needs terrain and config objects";
            return false;
        }
        std::map<std::string, Heightmap> parsedMaps;
        for (const auto& kv : terrain->object) {
            const sdk::JsonValue& m = kv.second;
            if (!m.is_object()) {
                errorOut = "terrain entry must be an object";
                return false;
            }
            const sdk::JsonValue* ox = m.field("origin_x");
            const sdk::JsonValue* oz = m.field("origin_z");
            const sdk::JsonValue* cell = m.field("cell");
            const sdk::JsonValue* cols = m.field("cols");
            const sdk::JsonValue* rows = m.field("rows");
            const sdk::JsonValue* heights = m.field("heights");
            if (ox == nullptr || oz == nullptr || cell == nullptr ||
                cols == nullptr || rows == nullptr || heights == nullptr ||
                ox->kind != sdk::JsonValue::Kind::Number ||
                oz->kind != sdk::JsonValue::Kind::Number ||
                cell->kind != sdk::JsonValue::Kind::Number ||
                cols->kind != sdk::JsonValue::Kind::Number ||
                rows->kind != sdk::JsonValue::Kind::Number ||
                heights->kind != sdk::JsonValue::Kind::Array) {
                errorOut = "terrain entry malformed";
                return false;
            }
            Heightmap hm;
            hm.origin_x = ox->number;
            hm.origin_z = oz->number;
            hm.cell = cell->number;
            hm.cols = static_cast<int>(cols->number);
            hm.rows = static_cast<int>(rows->number);
            for (const sdk::JsonValue& h : heights->array) {
                if (h.kind != sdk::JsonValue::Kind::Number) {
                    errorOut = "heights must be numbers";
                    return false;
                }
                hm.heights.push_back(h.number);
            }
            if (hm.cols <= 0 || hm.rows <= 0 ||
                hm.heights.size() !=
                    static_cast<std::size_t>(hm.cols) * hm.rows ||
                hm.cell <= 0.0) {
                errorOut = "terrain entry invalid dimensions";
                return false;
            }
            parsedMaps[kv.first] = std::move(hm);
        }
        Config parsedCfg;
        const sdk::JsonValue* root = config->field("root");
        const sdk::JsonValue* feet = config->field("feet");
        if (root != nullptr) {
            if (root->kind != sdk::JsonValue::Kind::String) {
                errorOut = "config.root must be a string";
                return false;
            }
            parsedCfg.root_bone = root->string;
        }
        if (feet != nullptr) {
            if (!feet->is_array()) {
                errorOut = "config.feet must be an array";
                return false;
            }
            for (const sdk::JsonValue& f : feet->array) {
                if (!f.is_object()) {
                    errorOut = "foot entry must be an object";
                    return false;
                }
                const sdk::JsonValue* bone = f.field("bone");
                const sdk::JsonValue* ox = f.field("ox");
                const sdk::JsonValue* oy = f.field("oy");
                const sdk::JsonValue* oz = f.field("oz");
                const sdk::JsonValue* go = f.field("ground_offset");
                if (bone == nullptr || ox == nullptr || oy == nullptr ||
                    oz == nullptr || go == nullptr ||
                    bone->kind != sdk::JsonValue::Kind::String ||
                    ox->kind != sdk::JsonValue::Kind::Number ||
                    oy->kind != sdk::JsonValue::Kind::Number ||
                    oz->kind != sdk::JsonValue::Kind::Number ||
                    go->kind != sdk::JsonValue::Kind::Number) {
                    errorOut = "foot entry malformed";
                    return false;
                }
                FootConfig fc;
                fc.bone = bone->string;
                fc.local_offset = {ox->number, oy->number, oz->number};
                fc.ground_offset = go->number;
                parsedCfg.feet.push_back(fc);
            }
        }
        maps_ = std::move(parsedMaps);
        config_ = std::move(parsedCfg);
        errorOut.clear();
        return true;
    }

private:
    std::map<std::string, Heightmap> maps_;
    Config config_;
};

}  // namespace

std::unique_ptr<ITerrainAdaptation> create_terrain_adaptation() {
    return std::unique_ptr<ITerrainAdaptation>(new TerrainAdaptation());
}

}  // namespace engine::animation
