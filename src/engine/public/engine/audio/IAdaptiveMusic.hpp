#pragma once
// IAdaptiveMusic — contrato público de música adaptativa data-driven
// (agente 4 §7 item 75).
//
// Camadas/estados/sequenciamento/stingers determinísticos e headless: NÃO
// toca dispositivo de áudio — é o CORE de mixagem adaptativa (crossfade entre
// estados de gameplay por camadas, intensidade global, stingers pontuais) que
// o projeto liga ao backend de som. Puro e determinístico: SEM RNG, SEM
// relógio de parede, SEM estado global; o tempo só entra pelo `dt` passado a
// `tick()`. A mesma spec + sequência de chamadas produzem o mesmo estado e os
// mesmos eventos bit-exatos entre instâncias. JSON versionado all-or-nothing
// bit-exact.
//
// Modelo:
//   - layer: trilha de uma camada (ex.: drums/bass/melody). Cada estado define
//     o ganho [0,1] de cada camada (camadas ausentes = 0).
//   - state: estado de gameplay (ex.: explore/combat/stealth) com ganhos por
//     camada + tempo de crossfade em segundos. `set_state` inicia um crossfade
//     linear determinístico de `transition_s`; ao completar, emite o evento
//     `StateChanged`. transition 0 = salto instantâneo.
//   - intensity: multiplicador global [0,1] dos ganhos (dinâmica da música).
//   - stinger: acento pontual (ex.: hit, boss-arrival); `trigger_stinger`
//     emite um evento `StingerTriggered` com a camada alvo e intensidade.
//   - eventos: fila drenada por `drain_events()` (ordem determinística).

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace engine::audio {

struct MusicLayer {
    std::string id;
};

struct MusicState {
    std::string id;
    // Ganho [0,1] por layer (pares layer→gain; layers ausentes = 0).
    std::vector<std::pair<std::string, double>> layer_gains;
    double transition_s = 1.0;  // crossfade em segundos (>= 0)
};

struct MusicStinger {
    std::string id;
    std::string layer;      // layer alvo ("" = master/mix)
    double intensity = 1.0; // [0,1]
};

struct AdaptiveMusicSpec {
    std::vector<MusicLayer> layers;
    std::vector<MusicState> states;
    std::vector<MusicStinger> stingers;

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;  // bit-exact (%.9g)
};

enum class MusicEventKind { StateChanged, StingerTriggered };

struct MusicEvent {
    MusicEventKind kind = MusicEventKind::StateChanged;
    std::string id;       // state id (StateChanged) ou stinger id (Stinger)
    std::string layer;    // stinger: layer alvo
    double intensity = 0.0;
};

// Sistema de música adaptativa (core de mixagem/sequenciamento determinístico).
class IAdaptiveMusic {
public:
    virtual ~IAdaptiveMusic() = default;

    // Aplica a spec (all-or-nothing via AdaptiveMusicSpec::validate).
    virtual bool configure(const AdaptiveMusicSpec& spec, std::string& errorOut) = 0;

    // Inicia o crossfade para o estado (no-op se já é o atual; recusa estado
    // desconhecido). transition 0 = salto instantâneo com evento imediato.
    virtual bool set_state(const std::string& id, std::string& errorOut) = 0;
    virtual std::string current_state() const = 0;

    // Multiplicador global [0,1] dos ganhos.
    virtual bool set_intensity(double intensity, std::string& errorOut) = 0;

    // Agenda um stinger (recusa id desconhecido); o evento sai no drain.
    virtual bool trigger_stinger(const std::string& id, std::string& errorOut) = 0;

    // Avança os crossfades em `dt` (finito >= 0); emite StateChanged ao
    // completar.
    virtual bool tick(double dt, std::string& errorOut) = 0;

    // Ganho atual (com intensidade aplicada) de uma layer; 0 se desconhecida.
    virtual double layer_gain(const std::string& layer) const = 0;

    // Eventos pendentes (ordem determinística), esvaziando a fila.
    virtual std::vector<MusicEvent> drain_events() = 0;

    // Estado (estado atual + intensidade + ganhos) serializado bit-exact /
    // restaurado all-or-nothing (crossfade em voo é transitório — a
    // serialização captura o estado estacionário).
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IAdaptiveMusic).
std::unique_ptr<IAdaptiveMusic> create_adaptive_music();

}  // namespace engine::audio
