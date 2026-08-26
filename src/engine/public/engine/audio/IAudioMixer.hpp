#pragma once
// IAudioMixer — contrato público do mixer de áudio data-driven
// (agente 4 §7 item 73).
//
// Mixer/buses/roteamento determinístico e headless: NÃO toca dispositivo de
// áudio real — é o CORE de ganho/roteamento que o projeto liga ao backend de
// som. Puro e determinístico: SEM RNG, SEM relógio de parede, SEM estado
// global; o tempo só entra pelo `dt` passado a `tick()` (envelopes de
// sidechain). A mesma spec + níveis + sequência de dt produzem o mesmo estado
// bit-exato entre instâncias. JSON versionado all-or-nothing bit-exact.
//
// Modelo:
//   - bus: id único + `gain_db` (base) + `parent` (roteamento em árvore; "" =
//     raiz/master). O nível PRÉ-gain de um bus = seu input + Σ(nível PÓS-gain
//     dos filhos). O nível PÓS-gain = pre * db_to_linear(gain_efetivo).
//   - gain_efetivo(bus) = base + ducking (envelope de sidechain, dB).
//   - sidechain {source, target, threshold, duck_db, attack_s, release_s}:
//     quando o nível pré-gain de `source` cruza `threshold` (linear [0,1]), o
//     alvo é "duckado" em até `duck_db` (negativo = abaixa); ataque/release
//     são rampas lineares determinísticas em dB por segundo.
//   - snapshot: ganhos base nomeados, aplicáveis/restauráveis (all-or-nothing).

#include <memory>
#include <string>
#include <vector>

namespace engine::audio {

// dB <-> linear (referência de amplitude): 20*log10(linear).
double linear_to_db(double linear);   // linear <= 0 → -infinito (mín. representável)
double db_to_linear(double db);       // 10^(db/20)

struct AudioBus {
    std::string id;
    double gain_db = 0.0;   // ganho base em dB (0 = unidade)
    std::string parent;     // roteamento: "" = raiz/master
};

struct AudioSidechain {
    std::string source;    // bus cujo nível dispara o duck
    std::string target;    // bus que é atenuado
    double threshold = 0.5; // nível linear [0,1] que dispara o duck
    double duck_db = -12.0; // atenuação máxima do alvo (negativo = abaixa)
    double attack_s = 0.05; // rampa de entrada (segundos, >= 0)
    double release_s = 0.2; // rampa de saída (segundos, >= 0)
};

struct AudioSnapshot {
    std::string name;
    struct Gain {
        std::string bus;
        double gain_db = 0.0;
    };
    std::vector<Gain> gains;
};

struct AudioMixerSpec {
    std::vector<AudioBus> buses;
    std::vector<AudioSidechain> sidechains;
    std::vector<AudioSnapshot> snapshots;

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;  // bit-exact (%.9g)
};

// Mixer de áudio (core de ganho/roteamento determinístico).
class IAudioMixer {
public:
    virtual ~IAudioMixer() = default;

    // Aplica a spec (all-or-nothing via AudioMixerSpec::validate).
    virtual bool configure(const AudioMixerSpec& spec, std::string& errorOut) = 0;

    // Nível de INPUT de um bus (linear, clampado a [0,1]). O nível final de um
    // bus soma o input + os filhos (roteamento).
    virtual bool set_input(const std::string& bus, double level,
                           std::string& errorOut) = 0;

    // Avança os envelopes de sidechain em `dt` (finito >= 0).
    virtual bool tick(double dt, std::string& errorOut) = 0;

    // Ganho efetivo (base + ducking) em dB de um bus.
    virtual double gain_db(const std::string& bus) const = 0;

    // Nível PÓS-gain (linear [0,1]) de um bus, após o próprio ganho efetivo.
    virtual double bus_level(const std::string& bus) const = 0;

    // Nível de saída do master (a raiz da árvore), linear [0,1].
    virtual double master_level() const = 0;

    // Snapshot: aplica os ganhos base nomeados (all-or-nothing; bus
    // desconhecido recusa sem mutar).
    virtual bool apply_snapshot(const std::string& name, std::string& errorOut) = 0;

    // Estado (envelopes de duck + ganhos) serializado bit-exact / restaurado
    // all-or-nothing.
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando IAudioMixer).
std::unique_ptr<IAudioMixer> create_audio_mixer();

}  // namespace engine::audio
