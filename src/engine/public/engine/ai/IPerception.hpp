#pragma once
// IPerception — contrato público de percepção/sensores (agente 4 §3 item 34).
//
// Sensores de um agente de IA puros e determinísticos: SEM RNG, SEM relógio de
// parede, SEM estado global — as mesmas entradas (posição, frente, estímulos,
// dt) produzem as mesmas detecções e a mesma memória bit-exata entre
// instâncias. Self-contained (std apenas) + o Vec3 do contrato irmão
// `engine/ai/ISteering.hpp` (domínio `engine/ai`).
//
// Modelo de sensor:
//   - visão: estímulo dentro de `vision_range` E dentro do cone de FOV
//     (dot(normalize(dir), forward) >= cos(vision_half_angle_deg)); frente zero
//     desliga a visão (hearing/proximity continuam).
//   - audição: estímulo com distância <= hearing_range * loudness (loudness ∈
//     [0,1] escala o raio; loudness 0 = silencioso, só visão/proximidade).
//   - proximidade: estímulo com distância <= proximity_range (detectado mesmo
//     de costas).
//   - `max_range` é o teto global: nada é detectado além dele.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/ai/ISteering.hpp"

namespace engine::ai {

// Um estímulo no mundo (posição + propriedades de detecção).
struct PerceptionStimulus {
    uint32_t id = 0;
    Vec3 position;
    float loudness = 1.0f;  // [0,1] — escala o raio de audição
    bool hostile = false;   // alimenta nearest_threat
    std::string kind;       // tag livre ("player", "enemy", ...)
    // A2-114 (Agente 5): faction affiliation of the stimulus source and its
    // damage/threat level. Aditivo e opcional — o adapter usa `hostile` para
    // nearest_threat (a facção/IPerception continua compatível); o JOGO
    // preenche `faction`/`damage` dos dados reais do componente para os
    // sensores consumirem dano/facção, não só hostil/kind.
    std::string faction;    // id da facção/equipe do emissor ("" = desconhecida)
    float damage = 0.0f;    // dano/threat do emissor (>= 0; 0 = nenhum)
};

// Um estímulo detectado (snapshot determinístico do último update).
struct Detection {
    uint32_t id = 0;
    Vec3 position;
    bool via_vision = false;
    bool via_hearing = false;
    bool via_proximity = false;
    bool hostile = false;
    std::string kind;
    float distance = 0.0f;
    // A2-114 (Agente 5): mirrors the stimulus faction/damage.
    std::string faction;
    float damage = 0.0f;
};

// Configuração dos sensores. `load_from_json`/`validate` são all-or-nothing:
// config inválida recusa com diagnóstico e não altera o objeto.
struct PerceptionSpec {
    float vision_range = 16.0f;         // 0 = visão desligada
    float vision_half_angle_deg = 60.0f;  // (0, 90] — metade do FOV do cone
    float hearing_range = 24.0f;        // 0 = audição desligada
    float proximity_range = 2.0f;       // 0 = proximidade desligada
    float memory_ttl = 5.0f;            // > 0 — segundos que um estímulo persiste
    float max_range = 128.0f;           // > 0 — teto global de detecção

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;  // bit-exact (%.9g)
};

// Sensor/memória de percepção de um agente.
class IPerception {
public:
    virtual ~IPerception() = default;

    // Aplica a configuração (all-or-nothing via PerceptionSpec::validate).
    virtual bool configure(const PerceptionSpec& spec, std::string& errorOut) = 0;

    // Avança um frame de sensoriamento: re-detecta os estímulos a partir de
    // `agent_pos`/`agent_forward` (normalizado internamente), avança a memória
    // em `dt` (esquece estímulos além de memory_ttl). dt finito >= 0.
    virtual bool update(const Vec3& agent_pos, const Vec3& agent_forward,
                        const std::vector<PerceptionStimulus>& stimuli,
                        float dt, std::string& errorOut) = 0;

    // Detecções do último update, ordenadas por (distância, id) — determinístico.
    virtual std::vector<Detection> detections() const = 0;

    // A ameaça hostil mais próxima (empate → menor id). false = nenhuma.
    virtual bool nearest_threat(Detection& out) const = 0;

    // Ids em memória (detectados há <= memory_ttl), ordenados — determinístico.
    virtual std::vector<uint32_t> remembered_ids() const = 0;

    // Estado (memória) serializado bit-exact / restaurado all-or-nothing.
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IPerception).
std::unique_ptr<IPerception> create_perception();

}  // namespace engine::ai
