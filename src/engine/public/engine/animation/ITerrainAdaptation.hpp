#pragma once
// ITerrainAdaptation — adaptação procedural ao terreno (pés no chão).
//
// Contrato público self-contained (std only) do §4 item 2 (unidade final
// "adaptação procedural ao terreno"). Sem dependência de core — só os tipos
// vetoriais de IAnimCore. Modelo: heightmap em grade + configuração de pés
// relativos ao root.
//
// Semântica determinística:
//   set_heightmap(id, ox, oz, cell, heights[], cols, rows): grade retangular
//     com amostragem BILINEAR (clamp nas bordas); all-or-nothing (tamanho
//     exato cols*rows, cell > 0, alturas finitas).
//   configure(rootBone, feet): osso raiz + pés {bone, local_offset (bind,
//     relativo ao root), ground_offset}. All-or-nothing (root não vazio,
//     ossos únicos).
//   adapt(root_x, root_y, root_z): desce/sobe o root pelo MENOR ajuste que
//     pousa o pé mais profundo (d = min_f(H_f + off_f − (R + oy_f))); cada
//     pé recebe o delta vertical restante (≥ 0) p/ encostar no próprio
//     terreno. Devolve root_y ajustado + alvos/deltas por pé.
//
// Escopo §4 item 2: IK (#215) + retargeting (#217) + constraints (#218)
// + terreno (esta unidade) — item COMPLETO.

#include "engine/animation/IAnimCore.hpp"

#include <memory>
#include <string>
#include <vector>

namespace engine::animation {

struct FootConfig {
    std::string bone;
    AnimVec3 local_offset;   // posição do pé relativa ao root (bind)
    double ground_offset = 0.0;  // folga vertical do pé sobre o chão
};

struct FootGroundResult {
    std::string bone;
    double target_world_y = 0.0;  // Y do terreno na posição do pé
    double delta_y = 0.0;         // ajuste vertical do osso do pé
};

struct TerrainAdaptationResult {
    double root_y = 0.0;               // nova altura do root
    std::vector<FootGroundResult> feet;  // na ordem da configuração
};

// Adaptação procedural ao terreno — heightmap + pés, determinístico.
class ITerrainAdaptation {
public:
    virtual ~ITerrainAdaptation() = default;

    // Grade de alturas com amostragem bilinear; all-or-nothing.
    virtual bool set_heightmap(const std::string& terrainId, double origin_x,
                               double origin_z, double cell_size,
                               const std::vector<double>& heights, int cols,
                               int rows, std::string& errorOut) = 0;

    // Altura do terreno em (x, z) — bilinear com clamp nas bordas.
    virtual double height_at(const std::string& terrainId, double x, double z,
                             std::string& errorOut) const = 0;

    // Configura o osso raiz e a lista de pés (all-or-nothing).
    virtual bool configure(const std::string& rootBone,
                           const std::vector<FootConfig>& feet,
                           std::string& errorOut) = 0;

    // Alinha pés/root ao terreno (regra do menor ajuste — ver header).
    virtual TerrainAdaptationResult adapt(const std::string& terrainId,
                                          double root_x, double root_y,
                                          double root_z,
                                          std::string& errorOut) const = 0;

    // Registro completo serializado / restaurado all-or-nothing.
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando ITerrainAdaptation).
std::unique_ptr<ITerrainAdaptation> create_terrain_adaptation();

}  // namespace engine::animation
