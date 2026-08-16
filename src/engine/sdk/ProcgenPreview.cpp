// ProcgenPreview.cpp
//
// SDK adapter for engine/procgen/IProcgenPreview.hpp (META section 18 /
// FALTANTES item 14: headless textual/statistical preview of the procgen
// pipeline). This TU composes the public procgen contracts — noise graph,
// climate/biome, fast-wfc structures, delaunator/earcut parcels, shape
// grammar, heightmap erosion and mesh cooking — into bounded ASCII renders
// plus deterministic stats, so the editor can preview every generation
// domain without a GPU. No new backend: all generators are the same
// deterministic adapters the world uses, so a preview reflects exactly what
// generation would produce. Every render is a pure function of its inputs
// (bit-identical across instances).

#include "engine/procgen/IProcgenPreview.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace procgen {
namespace {

// ASCII ramp for continuous fields, index 0 = lowest. Ten steps keep the
// render readable at the default 48-column size.
const char kRamp[] = " .:-=+*#%@";

std::size_t clamp_size(std::size_t requested) {
    return std::max<std::size_t>(8, std::min<std::size_t>(96, requested));
}

std::string fmt(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

void add_stat(PreviewRender& out, const std::string& label,
              const std::string& value) {
    out.stats.push_back(PreviewStat{ label, value });
}

// Maps a normalized [0, 1] value to a ramp char (clamped).
char ramp(float t) {
    int idx = static_cast<int>(t * (sizeof(kRamp) - 2)) + 1;
    idx = std::max(1, std::min<int>(static_cast<int>(sizeof(kRamp)) - 2, idx));
    return kRamp[idx];
}

// Renders a float field (row-major, `size` x `size`) to ASCII rows,
// normalized by the field's own min/max (robust to any graph output range).
std::vector<std::string> render_field(const std::vector<float>& field,
                                      std::size_t size) {
    float lo = field[0], hi = field[0];
    for (const float v : field) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    const float span = (hi - lo) > 1e-6f ? (hi - lo) : 1.0f;
    std::vector<std::string> lines(size);
    for (std::size_t z = 0; z < size; ++z) {
        std::string row;
        row.reserve(size);
        for (std::size_t x = 0; x < size; ++x) {
            row.push_back(ramp((field[z * size + x] - lo) / span));
        }
        lines[z] = std::move(row);
    }
    return lines;
}

void put(std::vector<char>& grid, std::size_t size, int x, int z, char c) {
    if (x >= 0 && x < static_cast<int>(size) && z >= 0 &&
        z < static_cast<int>(size)) {
        grid[static_cast<std::size_t>(z) * size + static_cast<std::size_t>(x)] =
            c;
    }
}

void line_on_grid(std::vector<char>& grid, std::size_t size, int x0, int z0,
                  int x1, int z1, char c) {
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dz = -std::abs(z1 - z0), sz = z0 < z1 ? 1 : -1;
    int err = dx + dz;
    for (;;) {
        put(grid, size, x0, z0, c);
        if (x0 == x1 && z0 == z1) {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dz) {
            err += dz;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            z0 += sz;
        }
    }
}

std::vector<std::string> grid_lines(const std::vector<char>& grid,
                                    std::size_t size) {
    std::vector<std::string> lines(size);
    for (std::size_t z = 0; z < size; ++z) {
        lines[z] = std::string(grid.data() + z * size, size);
    }
    return lines;
}

// Deterministic splitmix64 (same family the erosion adapter uses) for the
// preview's seeded inputs.
std::uint64_t splitmix64(std::uint64_t& state) {
    state += 0x9e3779b97f4a7c15ull;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// Smooth ridge + seeded noise in [0, 1], a deterministic erosion input. The
// noise dominates the small-scale roughness (the ridge stays visible as a
// large-scale shape), so hydraulic erosion visibly reduces the roughness
// statistic while the mass is conserved.
void make_ridge_heightmap(std::size_t size, std::size_t seed,
                          std::vector<float>& out) {
    std::uint64_t state =
        0x9e3779b97f4a7c15ull ^ static_cast<std::uint64_t>(seed);
    out.resize(size * size);
    for (std::size_t z = 0; z < size; ++z) {
        for (std::size_t x = 0; x < size; ++x) {
            const float ridge = 0.5f +
                                0.10f * std::sin(static_cast<float>(x) * 0.4f) *
                                    std::cos(static_cast<float>(z) * 0.4f);
            const float noise =
                (static_cast<float>(splitmix64(state) >> 40) / 16777216.0f -
                 0.5f) *
                0.36f;
            out[z * size + x] =
                std::max(0.0f, std::min(1.0f, ridge + noise));
        }
    }
}

// Roughness = population standard deviation of the field.
double roughness(const std::vector<float>& field) {
    double mean = 0.0;
    for (const float v : field) {
        mean += v;
    }
    mean /= static_cast<double>(field.size());
    double acc = 0.0;
    for (const float v : field) {
        const double d = static_cast<double>(v) - mean;
        acc += d * d;
    }
    return std::sqrt(acc / static_cast<double>(field.size()));
}

double field_sum(const std::vector<float>& field) {
    double s = 0.0;
    for (const float v : field) {
        s += v;
    }
    return s;
}

}  // namespace

class ProcgenPreview final : public IProcgenPreview {
public:
    bool preview_terrain(const INoiseGraph& height, const IClimateSampler& climate,
                         const IBiomeRegistry& biomes, const PreviewOptions& opts,
                         PreviewRender& out, std::string& error) override {
        (void)error;
        const std::size_t size = clamp_size(opts.sampleSize);
        std::vector<float> field(size * size);
        float lo = 0.0f, hi = 0.0f;
        for (std::size_t z = 0; z < size; ++z) {
            for (std::size_t x = 0; x < size; ++x) {
                const float v = height.sample_2d(static_cast<float>(x),
                                                 static_cast<float>(z));
                field[z * size + x] = v;
                if (z == 0 && x == 0) {
                    lo = hi = v;
                } else {
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
            }
        }

        double mean = 0.0;
        for (const float v : field) {
            mean += v;
        }
        mean /= static_cast<double>(field.size());

        std::map<std::string, std::size_t> biomeCounts;
        for (std::size_t z = 0; z < size; ++z) {
            for (std::size_t x = 0; x < size; ++x) {
                const ClimatePoint c = climate.sample(static_cast<float>(x),
                                                      static_cast<float>(z));
                std::uint32_t idx = 0;
                std::string name = "unknown";
                BiomeDefinition def;
                if (biomes.biome_for(c, idx) && biomes.biome_definition(idx, def)) {
                    name = def.name;
                }
                biomeCounts[name]++;
            }
        }

        out.title = "terrain " + std::to_string(size) + "x" + std::to_string(size);
        out.lines = render_field(field, size);
        if (opts.showStats) {
            add_stat(out, "height_min", fmt(lo, 3));
            add_stat(out, "height_max", fmt(hi, 3));
            add_stat(out, "height_mean", fmt(mean, 3));
            std::string dist;
            for (const auto& kv : biomeCounts) {
                if (!dist.empty()) {
                    dist += ", ";
                }
                dist += kv.first + ":" + std::to_string(kv.second);
            }
            add_stat(out, "biomes", dist);
        }
        return true;
    }

    bool preview_structure(const StructureAssetSpec& asset,
                           const PreviewOptions& opts, PreviewRender& out,
                           std::string& error) override {
        const std::size_t size = clamp_size(opts.sampleSize);
        auto gen = create_structure_generator(asset, error);
        if (!gen) {
            return false;
        }
        StructureOutput so;
        if (!gen->generate(static_cast<int>(size), static_cast<int>(size), so,
                           error)) {
            return false;
        }
        const int w = so.width, h = so.height;
        out.title = "structure " + std::to_string(w) + "x" + std::to_string(h);
        out.lines.clear();
        for (int z = 0; z < h; ++z) {
            std::string row;
            row.reserve(static_cast<std::size_t>(w));
            for (int x = 0; x < w; ++x) {
                const std::uint32_t id = so.plan[static_cast<std::size_t>(z * w + x)];
                row.push_back(id == 0 ? '.' : kRamp[1 + (id % 9)]);
            }
            out.lines.push_back(std::move(row));
        }
        if (opts.showStats) {
            add_stat(out, "plan", std::to_string(w) + "x" + std::to_string(h));
            std::size_t solid = 0;
            std::map<std::uint32_t, std::size_t> hist;
            for (const std::uint32_t id : so.plan) {
                if (id != 0) {
                    solid++;
                    hist[id]++;
                }
            }
            add_stat(out, "solid_cells", std::to_string(solid));
            std::string hstr;
            for (const auto& kv : hist) {
                if (!hstr.empty()) {
                    hstr += ", ";
                }
                hstr += std::to_string(kv.first) + ":" + std::to_string(kv.second);
            }
            add_stat(out, "blocks", hstr);
        }
        return true;
    }

    bool preview_parcels(const RoadNetworkSpec& spec, const PreviewOptions& opts,
                         PreviewRender& out, std::string& error) override {
        const std::size_t size = clamp_size(opts.sampleSize);
        auto builder = create_road_network_builder();
        if (!builder->build(spec, error)) {
            return false;
        }
        const RoadNetwork& network = builder->network();
        auto parcellation = create_parcellation();
        std::vector<ParcelPolygon> parcels;
        if (!parcellation->parcels_from_network(network, parcels, error)) {
            return false;
        }

        // Rasterize roads onto the grid (points -> '+', edges -> '#').
        double minX = network.points[0].x, maxX = network.points[0].x;
        double minY = network.points[0].y, maxY = network.points[0].y;
        for (const ParcelPoint& p : network.points) {
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
        }
        const double spanX = (maxX - minX) > 1e-9 ? (maxX - minX) : 1.0;
        const double spanY = (maxY - minY) > 1e-9 ? (maxY - minY) : 1.0;
        const int pad = 1;
        const int g = static_cast<int>(size) - 2 * pad;
        std::vector<char> grid(size * size, ' ');
        for (const RoadEdge& e : network.edges) {
            const ParcelPoint& a = network.points[e.a];
            const ParcelPoint& b = network.points[e.b];
            const int x0 = pad + static_cast<int>(((a.x - minX) / spanX) * g);
            const int z0 = pad + static_cast<int>(((a.y - minY) / spanY) * g);
            const int x1 = pad + static_cast<int>(((b.x - minX) / spanX) * g);
            const int z1 = pad + static_cast<int>(((b.y - minY) / spanY) * g);
            line_on_grid(grid, size, x0, z0, x1, z1, '#');
        }
        for (const ParcelPoint& p : network.points) {
            const int x = pad + static_cast<int>(((p.x - minX) / spanX) * g);
            const int z = pad + static_cast<int>(((p.y - minY) / spanY) * g);
            put(grid, size, x, z, '+');
        }

        out.title = "parcels " + std::to_string(network.points.size()) + " pts / " +
                    std::to_string(network.edges.size()) + " roads / " +
                    std::to_string(parcels.size()) + " parcels";
        out.lines = grid_lines(grid, size);
        if (opts.showStats) {
            add_stat(out, "junctions", std::to_string(network.points.size()));
            add_stat(out, "roads", std::to_string(network.edges.size()));
            add_stat(out, "parcels", std::to_string(parcels.size()));
            double minArea = 0.0, maxArea = 0.0, sum = 0.0;
            for (std::size_t i = 0; i < parcels.size(); ++i) {
                const double a = parcels[i].area();
                if (i == 0) {
                    minArea = maxArea = a;
                } else {
                    minArea = std::min(minArea, a);
                    maxArea = std::max(maxArea, a);
                }
                sum += a;
            }
            if (!parcels.empty()) {
                add_stat(out, "area_min", fmt(minArea, 2));
                add_stat(out, "area_mean", fmt(sum / parcels.size(), 2));
                add_stat(out, "area_max", fmt(maxArea, 2));
                add_stat(out, "area_sum", fmt(sum, 2));
            }
        }
        return true;
    }

    bool preview_shape(const ShapeGrammar& grammar, const PreviewOptions& opts,
                       PreviewRender& out, std::string& error) override {
        const std::size_t size = clamp_size(opts.sampleSize);
        auto runner = create_shape_grammar_runner();
        GrammarResult result;
        if (!runner->run(grammar, result, error)) {
            return false;
        }

        // Bounding box over the emitted (solid) boxes.
        int minX = 0, maxX = 0, minY = 0, maxY = 0, minZ = 0, maxZ = 0;
        bool first = true;
        long long volumeSum = 0;
        for (const GrammarBox& b : result.boxes) {
            if (b.blockId == 0) {
                continue;
            }
            const long long vol = static_cast<long long>(b.maxX - b.minX) *
                                  (b.maxY - b.minY) * (b.maxZ - b.minZ);
            volumeSum += vol;
            if (first) {
                minX = b.minX;
                maxX = b.maxX;
                minY = b.minY;
                maxY = b.maxY;
                minZ = b.minZ;
                maxZ = b.maxZ;
                first = false;
            } else {
                minX = std::min(minX, b.minX);
                maxX = std::max(maxX, b.maxX);
                minY = std::min(minY, b.minY);
                maxY = std::max(maxY, b.maxY);
                minZ = std::min(minZ, b.minZ);
                maxZ = std::max(maxZ, b.maxZ);
            }
        }
        if (first) {
            error = "shape grammar: no solid boxes emitted";
            return false;
        }

        // Top-down occupancy, normalized into the grid.
        const int spanX = std::max(1, maxX - minX);
        const int spanZ = std::max(1, maxZ - minZ);
        const int pad = 1;
        const int g = static_cast<int>(size) - 2 * pad;
        std::vector<char> grid(size * size, ' ');
        for (const GrammarBox& b : result.boxes) {
            if (b.blockId == 0) {
                continue;
            }
            for (int x = b.minX; x < b.maxX; ++x) {
                for (int z = b.minZ; z < b.maxZ; ++z) {
                    const int gx = pad + ((x - minX) * g) / spanX;
                    const int gz = pad + ((z - minZ) * g) / spanZ;
                    put(grid, size, gx, gz, kRamp[1 + (b.blockId % 9)]);
                }
            }
        }

        out.title = "shape " + std::to_string(result.boxes.size()) + " boxes";
        out.lines = grid_lines(grid, size);
        if (opts.showStats) {
            add_stat(out, "boxes", std::to_string(result.boxes.size()));
            add_stat(out, "volume", std::to_string(volumeSum));
            add_stat(out, "bounds_x",
                     std::to_string(minX) + ".." + std::to_string(maxX));
            add_stat(out, "bounds_y",
                     std::to_string(minY) + ".." + std::to_string(maxY));
            add_stat(out, "bounds_z",
                     std::to_string(minZ) + ".." + std::to_string(maxZ));
        }
        return true;
    }

    bool preview_erosion(const ErosionSpec& spec, const PreviewOptions& opts,
                         PreviewRender& out, std::string& error) override {
        const std::size_t size = clamp_size(opts.sampleSize);
        std::vector<float> before;
        make_ridge_heightmap(size, opts.seed, before);
        Heightmap in;
        in.width = static_cast<int>(size);
        in.height = static_cast<int>(size);
        in.values = before;
        Heightmap after;
        auto erosion = create_heightmap_erosion();
        if (!erosion->erode(in, spec, after, error)) {
            return false;
        }

        out.title = "erosion " + std::to_string(size) + "x" + std::to_string(size);
        out.lines = render_field(before, size);
        out.lines.push_back("");
        const std::vector<std::string> afterLines = render_field(after.values, size);
        out.lines.insert(out.lines.end(), afterLines.begin(), afterLines.end());
        if (opts.showStats) {
            add_stat(out, "roughness_before", fmt(roughness(before), 4));
            add_stat(out, "roughness_after", fmt(roughness(after.values), 4));
            add_stat(out, "mass_delta",
                     fmt(std::fabs(field_sum(after.values) - field_sum(before)),
                         6));
        }
        return true;
    }

    bool preview_mesh(const CookedMesh& mesh, const CookOptions& options,
                      const PreviewOptions& opts, PreviewRender& out,
                      std::string& error) override {
        auto cooker = create_mesh_cooker();
        CookedMesh cooked;
        CookStats stats;
        if (!cooker->cook(mesh, options, cooked, stats, error)) {
            return false;
        }
        out.title = "mesh cooked";
        out.lines.clear();
        if (opts.showStats) {
            add_stat(out, "input_vertices", std::to_string(stats.inputVertices));
            add_stat(out, "input_indices", std::to_string(stats.inputIndices));
            add_stat(out, "output_vertices", std::to_string(stats.outputVertices));
            add_stat(out, "output_indices", std::to_string(stats.outputIndices));
            add_stat(out, "acmr", fmt(stats.acmr, 3));
            add_stat(out, "overdraw", fmt(stats.overdraw, 3));
            add_stat(out, "has_uvs", stats.hasUvs ? "yes" : "no");
        }
        return true;
    }
};

}  // namespace procgen
}  // namespace engine

namespace engine {
namespace procgen {

std::shared_ptr<IProcgenPreview> create_procgen_preview() {
    return std::make_shared<ProcgenPreview>();
}

}  // namespace procgen
}  // namespace engine
