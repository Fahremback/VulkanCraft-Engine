#pragma once

// IRayTracer — ray tracing OFFLINE em CPU (baking, ambient occlusion, consultas
// de geometria), com backend Embree 4.4.1 vendido self-contained dentro do
// vc_sdk_public (padrão BUG-010 — sem libs externas em runtime).
//
// Semântica (fixada empiricamente no gate embree_ray_tracer_tests):
//   * Triângulos SINGLE-SIDED: a face frontal é o lado para onde aponta o
//     normal right-hand-rule da winding (v0,v1,v2); rays vindos por trás
//     (dot(normal, dir) >= 0) são culled.
//   * Determinístico: mesmas entradas -> mesmos resultados bit-exatos
//     (mesmo seed/tarefa de build, sem threading interno de simulação).
//   * All-or-nothing: build inválido (0 triângulos / null) retorna false e
//     deixa o tracer num estado seguro onde consultas retornam hit=false.
//
// Rays são fornecidas NORMALIZADAS pelo chamador: t é distância verdadeira.

#include <cstdint>
#include <memory>

namespace vc::rendering {

// Triângulo de entrada (winding front-face right-hand: v0,v1,v2).
struct RayTracerTriangle {
    float v0[3];
    float v1[3];
    float v2[3];
};

// Ray normalizada (|dir| = 1): tMin/tMax são distâncias verdadeiras.
struct RayTracerRay {
    float ox, oy, oz;   // origem
    float dx, dy, dz;   // direção NORMALIZADA
    float tMin;         // distância mínima (>= 0)
    float tMax;         // distância máxima (ex.: 1e30f)
};

// Resultado de closest-hit.
struct RayTracerHit {
    bool hit;                 // true se alguma face frontal foi atingida em [tMin, tMax]
    float t;                  // distância do hit mais próximo
    int32_t primitiveIndex;   // índice do triângulo atingido
};

class IRayTracer {
public:
    virtual ~IRayTracer() = default;

    // Constrói a estrutura de aceleração a partir dos triângulos.
    // count == 0 ou triangles == nullptr -> false (all-or-nothing, sem estado parcial).
    virtual bool build(const RayTracerTriangle* triangles, int32_t count) = 0;

    // Consulta closest-hit (face frontal apenas) dentro de [tMin, tMax].
    // hit=false quando nada é atingido ou o tracer não foi construído com sucesso.
    virtual RayTracerHit closestHit(const RayTracerRay& ray) const = 0;

    // Consulta any-hit/occlusion: true se QUALQUER face frontal é atingida
    // dentro de [tMin, tMax] (não necessariamente a mais próxima).
    virtual bool occluded(const RayTracerRay& ray) const = 0;
};

// Factory do SDK (adapter EmbreeRayTracer.cpp no vc_sdk_public).
std::unique_ptr<IRayTracer> create_ray_tracer();

}  // namespace vc::rendering
