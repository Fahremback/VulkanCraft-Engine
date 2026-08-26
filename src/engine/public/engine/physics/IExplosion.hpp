// IExplosion — modelo determinístico de explosão (impulso/calor/dano/
// fragmentos). Primeiro contrato do domínio `engine/physics/`.
//
// Contrato público self-contained (std only) do §5 item 58 (componente
// CORE — a integração com voxel por budget e o impulso em corpos rígidos
// são unidades registradas). Sem dependência de motor físico: a física do
// blast é calculada aqui, headless e testável.
//
#pragma once
// Semântica determinística:
//   sample_at(d): falloff = (1 − d/radius)^falloff_power clampado em [0,1]
//     (d ≥ radius → 0; d < 0 → erro); impulse/heat/damage = valor · falloff.
//   fragments(seed): direções em esfera unitária via RNG determinístico
//     (xorshift32 encadeado a partir do seed — SEM RNG global), speed =
//     fragment_speed · (0.5 + 0.5·u), massa uniforme 1/fragments.
//     Mesmo seed → mesmos fragmentos (bit-exact).
//
// Escopo §5 item 58: blast CORE (esta unidade); impulso em corpos,
// alteração voxel por budget e propagação estrutural = unidades futuras.

#include "engine/animation/IAnimCore.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::physics {

// Reusa o vocabulário vetorial de engine::animation (sem depender do núcleo).
using engine::animation::AnimVec3;

struct ExplosionSpec {
    double radius = 5.0;         // raio de efeito (m, > 0)
    double impulse = 1000.0;     // impulso no epicentro (N·s, >= 0)
    double heat = 1000.0;        // calor no epicentro (>= 0)
    double damage = 100.0;       // dano estrutural no epicentro (>= 0)
    double falloff_power = 2.0;  // 1 = linear; 2 = decaimento quadrático
    int fragments = 16;          // nº de fragmentos (> 0)
    double fragment_speed = 20.0;  // velocidade máx dos fragmentos (>= 0)
};

struct ExplosionSample {
    double distance = 0.0;
    double falloff = 0.0;  // (1 − d/radius)^power, clamp [0,1]
    double impulse = 0.0;
    double heat = 0.0;
    double damage = 0.0;
};

struct ExplosionFragment {
    AnimVec3 direction;  // unitária (determinística do seed)
    double speed = 0.0;  // fragment_speed · (0.5 + 0.5·u)
    double mass = 0.0;   // 1/fragments (uniforme)
};

// Explosão determinística (sem RNG global/relógio).
class IExplosion {
public:
    virtual ~IExplosion() = default;

    // All-or-nothing: raio > 0; impulse/heat/damage/fragment_speed >= 0;
    // falloff_power >= 0; fragments > 0.
    virtual bool configure(const ExplosionSpec& spec,
                           std::string& errorOut) = 0;

    virtual const ExplosionSpec& spec() const = 0;

    // Amostra o blast na distância d do epicentro (erro p/ d < 0).
    virtual ExplosionSample sample_at(double distance,
                                      std::string& errorOut) const = 0;

    // Fragmentos determinísticos (mesmo seed → bit-exact).
    virtual std::vector<ExplosionFragment> fragments(
        uint64_t seed, std::string& errorOut) const = 0;

    // Registro completo serializado / restaurado all-or-nothing.
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IExplosion).
std::unique_ptr<IExplosion> create_explosion();

}  // namespace engine::physics
