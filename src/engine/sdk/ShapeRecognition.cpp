// ShapeRecognition.cpp — the only TU implementing IShapeRecognition.
//
// Reconhecimento de primitivos por RANSAC determinístico, do zero: plano
// (3 pontos), esfera (4 pontos → circunferência circunscrita) e caixa
// (AABB dos pontos restantes com aceite por fração de apoio). A cada rodada
// o melhor primitivo (maior support) com support >= minSupport é aceito e
// seus inliers removidos; repete até não restar primitivo viável.
//
// Determinismo: RNG splitmix64 semeado (sem RNG global), amostras e pontos
// percorridos em ordem fixa — mesma (nuvem, config) produz os mesmos
// primitivos e inliers (bit-exact).

#include "engine/physics/IShapeRecognition.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

namespace engine {
namespace physics {
namespace {

// splitmix64 determinístico (estado local, sem RNG global).
struct SplitMix64 {
    std::uint64_t state;
    explicit SplitMix64(std::uint64_t seed) : state(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
};

bool ShapeRecognitionConfig_validate(const ShapeRecognitionConfig& config,
                                     std::string& errorOut) {
    if (config.maxPoints < 1 || config.maxPoints > (1u << 20)) {
        errorOut = "shape config: maxPoints must be in [1, 1<<20]";
        return false;
    }
    if (config.maxIterations < 1 || config.maxIterations > (1u << 16)) {
        errorOut = "shape config: maxIterations must be in [1, 1<<16]";
        return false;
    }
    if (!(config.inlierThreshold > 0.0f) ||
        !std::isfinite(config.inlierThreshold)) {
        errorOut = "shape config: inlierThreshold must be finite and > 0";
        return false;
    }
    if (config.minSupport < 1 || config.minSupport > (1u << 20)) {
        errorOut = "shape config: minSupport must be in [1, 1<<20]";
        return false;
    }
    return true;
}

inline float distance_to_plane(const glm::vec3& p, const glm::vec3& n,
                               float d) {
    return std::fabs(glm::dot(n, p) - d);
}

class ShapeRecognition final : public IShapeRecognition {
public:
    const ShapeRecognitionConfig& config() const noexcept override {
        return config_;
    }

    bool configure(const ShapeRecognitionConfig& config,
                   std::string& errorOut) override {
        if (!ShapeRecognitionConfig_validate(config, errorOut)) return false;
        config_ = config;
        remaining_.clear();
        return true;
    }

    bool recognize(const std::vector<glm::vec3>& points,
                   std::vector<ShapePrimitive>& out,
                   std::string& errorOut) override {
        if (points.size() > config_.maxPoints) {
            errorOut = "shape: point cloud exceeds maxPoints (" +
                       std::to_string(config_.maxPoints) + ")";
            return false;
        }
        if (points.size() < 4) {
            errorOut = "shape: point cloud needs at least 4 points";
            return false;
        }
        out.clear();

        // Índices restantes (ordem de entrada), atualizados a cada rodada.
        std::vector<std::uint32_t> remaining(points.size());
        for (std::uint32_t i = 0; i < points.size(); ++i) remaining[i] = i;

        SplitMix64 rng(config_.seed);
        while (true) {
            if (remaining.size() < 4) break;  // nada a encaixar
            ShapePrimitive best;
            std::vector<std::uint32_t> bestInliers;
            const std::uint32_t n = static_cast<std::uint32_t>(remaining.size());

            // --- Plano (3 pontos) ---
            for (std::uint32_t trial = 0; trial < config_.maxIterations; ++trial) {
                const std::uint32_t i0 = remaining[rng.next() % n];
                const std::uint32_t i1 = remaining[rng.next() % n];
                const std::uint32_t i2 = remaining[rng.next() % n];
                if (i0 == i1 || i0 == i2 || i1 == i2) continue;
                const glm::vec3& a = points[i0];
                const glm::vec3& b = points[i1];
                const glm::vec3& c = points[i2];
                const glm::vec3 nrm = glm::cross(b - a, c - a);
                const float len = glm::length(nrm);
                if (len < 1e-9f) continue;  // colinear
                const glm::vec3 normal = nrm / len;
                const float d = glm::dot(normal, a);
                std::vector<std::uint32_t> inliers;
                for (std::uint32_t i = 0; i < n; ++i) {
                    if (distance_to_plane(points[remaining[i]], normal, d) <=
                        config_.inlierThreshold)
                        inliers.push_back(remaining[i]);
                }
                if (inliers.size() > bestInliers.size()) {
                    best.kind = PrimitiveKind::Plane;
                    best.normal = normal;
                    best.support = static_cast<std::uint32_t>(inliers.size());
                    bestInliers = std::move(inliers);
                }
            }
            if (bestInliers.size() >= config_.minSupport) {
                glm::vec3 center(0.0f);
                for (const std::uint32_t i : bestInliers) center += points[i];
                center /= static_cast<float>(bestInliers.size());
                best.center = center;
                best.inlierIndices = std::move(bestInliers);
                out.push_back(best);
                remove_points(best.inlierIndices, remaining);
                continue;
            }

            // --- Esfera (4 pontos → circumesfera) ---
            best = ShapePrimitive();
            bestInliers.clear();
            for (std::uint32_t trial = 0; trial < config_.maxIterations; ++trial) {
                std::uint32_t idx[4];
                bool ok = true;
                for (int k = 0; k < 4; ++k) {
                    idx[k] = remaining[rng.next() % n];
                    for (int m = 0; m < k; ++m)
                        if (idx[k] == idx[m]) ok = false;
                }
                if (!ok) continue;
                glm::vec3 center;
                float radius = 0.0f;
                if (!fit_sphere(points[idx[0]], points[idx[1]], points[idx[2]],
                                points[idx[3]], center, radius))
                    continue;  // coplanar/degene
                std::vector<std::uint32_t> inliers;
                for (std::uint32_t i = 0; i < n; ++i) {
                    const float dist =
                        std::fabs(glm::length(points[remaining[i]] - center) -
                                  radius);
                    if (dist <= config_.inlierThreshold)
                        inliers.push_back(remaining[i]);
                }
                if (inliers.size() > bestInliers.size()) {
                    best.kind = PrimitiveKind::Sphere;
                    best.center = center;
                    best.radius = radius;
                    best.support = static_cast<std::uint32_t>(inliers.size());
                    bestInliers = std::move(inliers);
                }
            }
            if (bestInliers.size() >= config_.minSupport) {
                best.inlierIndices = std::move(bestInliers);
                out.push_back(best);
                remove_points(best.inlierIndices, remaining);
                continue;
            }

            // --- Caixa (AABB dos restantes, aceita por fração de apoio) ---
            glm::vec3 minP = points[remaining[0]];
            glm::vec3 maxP = points[remaining[0]];
            for (std::uint32_t i = 1; i < n; ++i) {
                minP = glm::min(minP, points[remaining[i]]);
                maxP = glm::max(maxP, points[remaining[i]]);
            }
            {
                std::vector<std::uint32_t> inliers;
                const float t = config_.inlierThreshold;
                for (std::uint32_t i = 0; i < n; ++i) {
                    const glm::vec3& p = points[remaining[i]];
                    // Distância ao ponto mais próximo da SUPERFÍCIE do AABB
                    // (mínimo sobre os 6 planos): pontos internos longe das
                    // faces NÃO contam como apoio da caixa.
                    const float dx = std::min(p.x - minP.x, maxP.x - p.x);
                    const float dy = std::min(p.y - minP.y, maxP.y - p.y);
                    const float dz = std::min(p.z - minP.z, maxP.z - p.z);
                    const float dist = std::min(std::min(dx, dy), dz);
                    if (dist <= t) inliers.push_back(remaining[i]);
                }
                // Aceita apenas se a fração de apoio for alta (nuvem com forma
                // de caixa), evitando engolir sobras.
                const float ratio =
                    static_cast<float>(inliers.size()) / static_cast<float>(n);
                if (inliers.size() >= config_.minSupport && ratio >= 0.8f) {
                    ShapePrimitive box;
                    box.kind = PrimitiveKind::Box;
                    box.center = (minP + maxP) * 0.5f;
                    box.extents = (maxP - minP) * 0.5f;
                    box.support = static_cast<std::uint32_t>(inliers.size());
                    box.inlierIndices = std::move(inliers);
                    out.push_back(box);
                    remove_points(box.inlierIndices, remaining);
                    continue;
                }
            }

            break;  // nenhum primitivo viável
        }

        remaining_ = std::move(remaining);
        return true;
    }

    const std::vector<std::uint32_t>& remaining_indices() const noexcept override {
        return remaining_;
    }

private:
    // Circumesfera de 4 pontos: o centro c (relativo a p0) satisfaz
    //   c·(p_i − p_0) = |p_i − p_0|² / 2   para i = 1, 2, 3.
    // A matriz N tem LINHAS (p_i − p_0)ᵀ; resolvemos N·c = b por inversão.
    // Falha (false) para pontos coplanares (determinante ~ 0) ou raio ~ 0.
    static bool fit_sphere(const glm::vec3& p0, const glm::vec3& p1,
                           const glm::vec3& p2, const glm::vec3& p3,
                           glm::vec3& center, float& radius) {
        const glm::vec3 v1 = p1 - p0;
        const glm::vec3 v2 = p2 - p0;
        const glm::vec3 v3 = p3 - p0;
        const float det = glm::dot(v1, glm::cross(v2, v3));
        if (std::fabs(det) < 1e-12f) return false;
        const glm::mat3 N(glm::vec3(v1.x, v2.x, v3.x),
                          glm::vec3(v1.y, v2.y, v3.y),
                          glm::vec3(v1.z, v2.z, v3.z));
        const glm::vec3 b(glm::dot(v1, v1) * 0.5f, glm::dot(v2, v2) * 0.5f,
                          glm::dot(v3, v3) * 0.5f);
        const glm::mat3 inv = glm::inverse(N);
        const glm::vec3 c = inv * b;
        center = p0 + c;
        radius = glm::length(c);
        return radius > 1e-9f;
    }

    static void remove_points(const std::vector<std::uint32_t>& inliers,
                              std::vector<std::uint32_t>& remaining) {
        std::vector<std::uint32_t> keep;
        keep.reserve(remaining.size());
        for (const std::uint32_t i : remaining) {
            if (std::find(inliers.begin(), inliers.end(), i) == inliers.end())
                keep.push_back(i);
        }
        remaining = std::move(keep);
    }

    ShapeRecognitionConfig config_;
    std::vector<std::uint32_t> remaining_;
};

}  // namespace

bool ShapeRecognitionConfig::valid(std::string& errorOut) const {
    return ShapeRecognitionConfig_validate(*this, errorOut);
}

bool ShapeRecognitionConfig::load_from_json(const std::string& json,
                                            std::string& errorOut) {
    ShapeRecognitionConfig candidate = *this;
    bool any = false;
    std::size_t pos = 0;
    while (pos < json.size()) {
        const std::size_t kStart = json.find('"', pos);
        if (kStart == std::string::npos) break;
        const std::size_t kEnd = json.find('"', kStart + 1);
        if (kEnd == std::string::npos) break;
        const std::string key = json.substr(kStart + 1, kEnd - kStart - 1);
        const std::size_t colon = json.find(':', kEnd);
        if (colon == std::string::npos) break;
        const std::size_t vStart = json.find_first_not_of(" \t\r\n", colon + 1);
        if (vStart == std::string::npos) break;
        const std::size_t vEnd = json.find_first_of(",}", vStart);
        const std::string value =
            json.substr(vStart, vEnd == std::string::npos ? std::string::npos
                                                          : vEnd - vStart);
        if (key == "maxPoints") {
            candidate.maxPoints = static_cast<std::uint32_t>(
                std::strtoul(value.c_str(), nullptr, 10));
            any = true;
        } else if (key == "maxIterations") {
            candidate.maxIterations = static_cast<std::uint32_t>(
                std::strtoul(value.c_str(), nullptr, 10));
            any = true;
        } else if (key == "inlierThreshold") {
            candidate.inlierThreshold = std::strtof(value.c_str(), nullptr);
            any = true;
        } else if (key == "minSupport") {
            candidate.minSupport = static_cast<std::uint32_t>(
                std::strtoul(value.c_str(), nullptr, 10));
            any = true;
        } else if (key == "seed") {
            candidate.seed = std::strtoull(value.c_str(), nullptr, 10);
            any = true;
        }
        pos = vEnd == std::string::npos ? json.size() : vEnd + 1;
    }
    if (!any) {
        errorOut = "shape config: no recognized keys";
        return false;
    }
    if (!ShapeRecognitionConfig_validate(candidate, errorOut)) return false;
    *this = candidate;
    return true;
}

std::string ShapeRecognitionConfig::to_json() const {
    std::string out = "{";
    out += "\"maxPoints\":" + std::to_string(maxPoints) + ",";
    out += "\"maxIterations\":" + std::to_string(maxIterations) + ",";
    out += "\"inlierThreshold\":" + std::to_string(inlierThreshold) + ",";
    out += "\"minSupport\":" + std::to_string(minSupport) + ",";
    out += "\"seed\":" + std::to_string(seed);
    out += "}";
    return out;
}

std::unique_ptr<IShapeRecognition> create_shape_recognition(
    std::string& errorOut) {
    auto impl = std::make_unique<ShapeRecognition>();
    if (!impl) {
        errorOut = "shape: allocation failed";
        return nullptr;
    }
    return impl;
}

std::unique_ptr<IShapeRecognition> create_shape_recognition_json(
    const std::string& jsonText, std::string& errorOut) {
    ShapeRecognitionConfig config;
    if (!config.load_from_json(jsonText, errorOut)) return nullptr;
    auto impl = std::make_unique<ShapeRecognition>();
    if (!impl->configure(config, errorOut)) return nullptr;
    return impl;
}

}  // namespace physics
}  // namespace engine
