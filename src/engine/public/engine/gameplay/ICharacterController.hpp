#pragma once
// ICharacterController — resolução cinemática de movimento de personagem
// sobre terreno (degraus, rampas, snap, água). Primeiro contrato do §4
// item 54 (character controller estável — unidade CORE).
//
// O contrato NÃO conhece física nem voxel: o chamador fornece amostras de
// altura do terreno (heightmap) ao redor do personagem e o controlador
// resolve o movimento desejado respeitando:
//   - maxStepHeight: sobe degraus até essa altura (step-up);
//   - stepDownDistance: desce/snap ao chão após o movimento (step-down);
//   - maxSlope: recusa subir inclinações acima do limite (projetado na
//     direção de subida máxima permitida);
//   - water: abaixo da superfície, arrasto exponencial + flutuação linear
//     pela profundidade (drag/buoyancy determinísticos).
//
// Determinístico e headless (std only). Sem RNG, sem estado global.

#include <cstdint>
#include <memory>
#include <string>

namespace engine::gameplay {

struct CharacterConfig {
    float maxStepHeight{ 0.5f };     // degrau máximo que o personagem sobe (>= 0)
    float stepDownDistance{ 0.6f };  // snap máximo ao chão ao descer (>= 0)
    float maxSlopeDegrees{ 50.0f };  // inclinação máxima aceita (0..90)
    float waterSurfaceY{ 0.0f };     // nível da água (world Y)
    float waterDrag{ 2.0f };         // arrasto exponencial por segundo (>= 0)
    float waterBuoyancy{ 3.0f };     // aceleração de flutuação por unidade de
                                     // profundidade (>= 0)
};

// Altura do terreno num ponto (world XZ) — fornecida pelo chamador.
struct TerrainSample {
    float x{ 0.0f };
    float z{ 0.0f };
    float height{ 0.0f };  // altura do chão sólido nesse ponto
};

// Resultado da resolução de um passo de movimento.
struct MoveResult {
    float newY{ 0.0f };         // altura final do personagem
    bool steppedUp{ false };    // subiu um degrau (<= maxStepHeight)
    bool snappedDown{ false };  // desceu/snap ao chão (<= stepDownDistance)
    bool slopeBlocked{ false }; // movimento horizontal reduzido por inclinação
    bool inWater{ false };      // posição final abaixo da superfície da água
};

class ICharacterController {
public:
    virtual ~ICharacterController() = default;

    virtual void set_config(const CharacterConfig& config) = 0;
    virtual CharacterConfig config() const = 0;

    // Resolve o movimento de um tick. `fromX/fromY/fromZ` é a posição atual
    // do personagem, `targetX/targetZ` a posição horizontal desejada após o
    // movimento, e `samples` as alturas do terreno ao redor (o controlador
    // usa a amostra mais próxima do alvo; vazio = sem chão, queda livre
    // limitada por stepDownDistance). A distância horizontal percorrida
    // (from→target) define a inclinação: rise/dx acima do limite projeta a
    // altura em maxSlope. Retorna a nova altura e os flags do passo.
    virtual MoveResult move(float fromX, float fromY, float fromZ,
                            float targetX, float targetZ,
                            const TerrainSample* samples, std::size_t sampleCount) = 0;

    // Aplica água: retorna a nova velocidade vertical após arrasto (por
    // profundidade) e flutuação. `depth` >= 0 abaixo da superfície.
    virtual float water_velocity(float velocityY, float depth, float dt) const = 0;
};

std::unique_ptr<ICharacterController> create_character_controller();

}  // namespace engine::gameplay
