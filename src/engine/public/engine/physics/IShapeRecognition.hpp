// IShapeRecognition.hpp
//
// PUBLIC seam para reconhecimento/segmentação de formas primitivas em nuvens
// de pontos. É a contraparte headless/determinística do catálogo shape-ml
// (reconhecimento de forma) — implementada do zero no SDK, sem dependência
// externa, com RANSAC determinístico (mesma família de algoritmos).
//
// Uso típico: segmentar um scan de voxel/malha em planos (chão, paredes),
// esferas (projéteis, planetas), caixas (construções) — alimenta snapping de
// editor, cooking de colisores e análise de ambientes.
//
// Determinismo: o RNG interno é splitmix64 semeado; o RANSAC percorre
// amostras e pontos em ordem fixa — mesma (nuvem, config) produz os mesmos
// primitivos e os mesmos conjuntos de inliers (bit-exact).

#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace physics {

enum class PrimitiveKind : std::uint8_t {
    Plane,   // normal + centro (ponto médio dos inliers)
    Sphere,  // centro + raio
    Box      // centro + extents (AABB dos inliers)
};

// Um primitivo detectado: parâmetros + índices dos pontos que o sustentam.
struct ShapePrimitive {
    PrimitiveKind kind{ PrimitiveKind::Plane };
    glm::vec3 center{ 0.0f, 0.0f, 0.0f };
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };  // Plane
    float radius{ 0.0f };                   // Sphere
    glm::vec3 extents{ 0.0f, 0.0f, 0.0f };  // Box
    std::uint32_t support{ 0 };             // nº de inliers
    std::vector<std::uint32_t> inlierIndices;  // índices na nuvem de entrada
};

// Configuração do RANSAC (all-or-nothing: valores fora do range são recusados
// com diagnóstico, nunca clampeados).
struct ShapeRecognitionConfig {
    std::uint32_t maxPoints{ 100000 };   // cap de pontos [1, 1<<20]
    std::uint32_t maxIterations{ 256 };  // amostras RANSAC por tentativa [1, 1<<16]
    float inlierThreshold{ 0.02f };      // tolerância de distância (m, > 0)
    std::uint32_t minSupport{ 8 };       // inliers mínimos p/ aceitar [1, 1<<20]
    std::uint64_t seed{ 1 };             // RNG determinístico

    bool valid(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;
};

class IShapeRecognition {
public:
    virtual ~IShapeRecognition() = default;

    virtual bool configure(const ShapeRecognitionConfig& config,
                           std::string& errorOut) = 0;
    virtual const ShapeRecognitionConfig& config() const noexcept = 0;

    // Segmenta a nuvem: repete RANSAC (plano → esfera → caixa) sobre os
    // pontos restantes, removendo os inliers de cada primitivo aceito
    // (support >= minSupport), até não restar primitivo viável. `out` recebe
    // os primitivos em ordem de detecção. Os índices não atribuídos ficam
    // disponíveis via remaining_indices(). Determinístico.
    virtual bool recognize(const std::vector<glm::vec3>& points,
                           std::vector<ShapePrimitive>& out,
                           std::string& errorOut) = 0;

    // Índices (na nuvem da última recognize) não atribuídos a nenhum
    // primitivo.
    virtual const std::vector<std::uint32_t>& remaining_indices() const noexcept = 0;
};

// Fábrica do adapter (o único TU que implementa IShapeRecognition).
std::unique_ptr<IShapeRecognition> create_shape_recognition(
    std::string& errorOut);
std::unique_ptr<IShapeRecognition> create_shape_recognition_json(
    const std::string& jsonText, std::string& errorOut);

}  // namespace physics
}  // namespace engine
