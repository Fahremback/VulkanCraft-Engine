// RayBakeMesh.cpp — Agente 1 (task_plan C.4 embree — caminho de baking no
// cooker criado do zero). Deterministic hemisphere ambient-occlusion bake over
// query points using the native IRayTracer (Embree backend). Pure, headless.
#include "engine/rendering/IRayBakeMesh.hpp"
#include "engine/rendering/IRayTracer.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace vc::rendering {

namespace {

std::uint64_t mix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
float unit01(std::uint64_t s) noexcept {
    return static_cast<float>((s >> 32) * (1.0 / 4294967296.0));
}

bool parseJson(const std::string& json, RayBakeConfig& out, std::string& err) {
    auto find = [&json](const char* key) -> std::string {
        std::string k = std::string("\"") + key + "\"";
        std::size_t p = json.find(k);
        if (p == std::string::npos) return {};
        p = json.find(':', p + k.size());
        if (p == std::string::npos) return {};
        std::string v; p++;
        while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) p++;
        while (p < json.size() && json[p] != ',' && json[p] != '}' && json[p] != ' ') { v += json[p]; p++; }
        return v;
    };
    (void)out; // fields parsed below
    std::string s = find("samples"); if (!s.empty()) out.samples = static_cast<std::uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
    s = find("maxDistance"); if (!s.empty()) out.maxDistance = std::strtof(s.c_str(), nullptr);
    s = find("seed"); if (!s.empty()) out.seed = static_cast<std::uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
    s = find("version"); if (!s.empty() && std::strtoul(s.c_str(), nullptr, 10) != 1u) { err = "version must be 1"; return false; }
    if (!out.valid(err)) return false;
    return true;
}

class RayBakeMesh final : public IRayBakeMesh {
public:
    RayBakeMesh() : config_(RayBakeConfig{}) {}
    ~RayBakeMesh() override = default;

    bool configure(const RayBakeConfig& config, std::string& errorOut) override {
        if (!config.valid(errorOut)) return false;
        config_ = config;
        return true;
    }
    const RayBakeConfig& config() const noexcept override { return config_; }
    bool configure_json(const std::string& jsonText, std::string& errorOut) override {
        RayBakeConfig p;
        if (!parseJson(jsonText, p, errorOut)) return false;
        return configure(p, errorOut);
    }
    std::string config_to_json() const override {
        std::ostringstream o;
        o << "{ \"version\": 1, \"samples\": " << config_.samples
          << ", \"maxDistance\": " << config_.maxDistance
          << ", \"seed\": " << config_.seed << " }";
        return o.str();
    }

    bool build(const RayTracerTriangle* tris, std::int32_t count,
               std::string& errorOut) override {
        tracer_ = create_ray_tracer();
        if (!tracer_ || !tracer_->build(tris, count)) {
            tracer_.reset();
            errorOut = "build failed (null/empty/invalid scene)";
            return false;
        }
        return true;
    }

    bool bake(const float* origins, std::size_t points,
              std::vector<RayBakeSample>& output,
              std::string& errorOut) const override {
        // Default hemisphere up = +Y (backward compatible with the original
        // bake()). The normal-aware path does the per-sample TBN.
        if (!tracer_ || !origins || points == 0) {
            errorOut = "scene not built or no query points";
            return false;
        }
        std::vector<float> up(points * 3, 0.0f);
        for (std::size_t i = 0; i < points; ++i) up[i * 3 + 1] = 1.0f;
        return bake_normals(origins, up.data(), points, output, errorOut);
    }

    bool bake_normals(const float* origins, const float* normals,
                      std::size_t points,
                      std::vector<RayBakeSample>& output,
                      std::string& errorOut) const override {
        if (!tracer_ || !origins || !normals || points == 0) {
            errorOut = "scene not built or no query points";
            return false;
        }
        output.resize(points);
        const float maxD = config_.maxDistance;
        for (std::size_t i = 0; i < points; ++i) {
            const float ox = origins[i * 3 + 0];
            const float oy = origins[i * 3 + 1];
            const float oz = origins[i * 3 + 2];
            // Normal da superfície neste ponto: hemisfério segue a normal
            // (TBN por amostra), não +Y fixo — correto para paredes/slopes.
            float nx = normals[i * 3 + 0];
            float ny = normals[i * 3 + 1];
            float nz = normals[i * 3 + 2];
            const float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (nlen < 1e-6f) { nx = 0.0f; ny = 1.0f; nz = 0.0f; }
            else { const float inv = 1.0f / nlen; nx *= inv; ny *= inv; nz *= inv; }
            // Base ortonormal (tangente, bitangente, normal). Escolhe um eixo
            // auxiliar determinístico não paralelo à normal.
            float ax = 0.0f, ay = 1.0f, az = 0.0f;
            if (std::fabs(ny) > 0.95f) { ax = 1.0f; ay = 0.0f; }
            float tx = ay * nz - az * ny;
            float ty = az * nx - ax * nz;
            float tz = ax * ny - ay * nx;
            const float tlen = std::sqrt(tx * tx + ty * ty + tz * tz);
            if (tlen < 1e-6f) { tx = 1.0f; ty = 0.0f; tz = 0.0f; }
            else { const float inv = 1.0f / tlen; tx *= inv; ty *= inv; tz *= inv; }
            // bitangente = cross(normal, tangente)
            const float bx = ny * tz - nz * ty;
            const float by = nz * tx - nx * tz;
            const float bz = nx * ty - ny * tx;

            std::uint64_t s = mix64(config_.seed + static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ull);
            int hits = 0;
            double distAcc = 0.0;
            for (std::uint32_t j = 0; j < config_.samples; ++j) {
                // Cosine-weighted hemisphere no frame local, seeded/stratified.
                const float u1 = unit01(mix64(s + j * 3u + 1u));
                const float u2 = unit01(mix64(s + j * 3u + 2u));
                const float r = std::sqrt(std::max(u1, 0.0f));
                const float phi = u2 * 2.0f * 3.14159265358979f;
                const float lx = r * std::cos(phi);
                const float lz = r * std::sin(phi);
                const float ly = std::sqrt(std::max(1.0f - r * r, 0.0f));
                // Converte o raio local para o espaço do mundo via TBN.
                RayTracerRay ray;
                ray.ox = ox; ray.oy = oy; ray.oz = oz;
                ray.dx = tx * lx + bx * lz + nx * ly;
                ray.dy = ty * lx + by * lz + ny * ly;
                ray.dz = tz * lx + bz * lz + nz * ly;
                const float dlen = std::sqrt(ray.dx * ray.dx + ray.dy * ray.dy + ray.dz * ray.dz);
                if (dlen < 1e-6f) continue;
                ray.dx /= dlen; ray.dy /= dlen; ray.dz /= dlen;
                ray.tMin = 1e-3f;
                ray.tMax = maxD;
                // QUERY ÚNICA: closestHit deriva a oclusão do próprio resultado
                // (antes, occluded() + closestHit() faziam DUAS travessias da
                // estrutura de aceleração por raio ocluído).
                const RayTracerHit h = tracer_->closestHit(ray);
                if (h.hit) {
                    ++hits;
                    distAcc += static_cast<double>(h.t);
                }
            }
            const float open = 1.0f - static_cast<float>(hits) / static_cast<float>(config_.samples);
            output[i].occlusion = std::max(0.0f, std::min(1.0f, open));
            double mean = hits > 0 ? distAcc / static_cast<double>(hits) : static_cast<double>(maxD);
            output[i].meanDistance = static_cast<float>(mean);
        }
        return true;
    }

private:
    RayBakeConfig config_;
    std::unique_ptr<IRayTracer> tracer_;
};

}  // namespace

bool RayBakeConfig::valid(std::string& errorOut) const {
    if (samples < 4 || samples > 512) { errorOut = "samples must be in [4, 512]"; return false; }
    if (!(maxDistance >= 1.0f && maxDistance <= 4096.0f)) { errorOut = "maxDistance must be in [1, 4096]"; return false; }
    if (seed == 0) { errorOut = "seed must be non-zero"; return false; }
    return true;
}

std::unique_ptr<IRayBakeMesh> create_ray_bake_mesh(std::string& errorOut) {
    auto impl = std::make_unique<RayBakeMesh>();
    if (!impl) { errorOut = "RayBakeMesh: allocation failed"; return nullptr; }
    return impl;
}

}  // namespace vc::rendering