#pragma once
// ISpatialAudio — contrato público de áudio espacial data-driven
// (agente 4 §7 item 74).
//
// Espacialização 3D determinística e headless: NÃO toca dispositivo de áudio —
// é o CORE de posicionamento (atenuação por distância, panning equal-power,
// oclusão, reverb zones, prioridade/virtualização) que o projeto liga ao
// backend de som. Puro e determinístico: SEM RNG, SEM relógio de parede, SEM
// estado global; a mesma spec + listener + fontes + max_voices produzem os
// mesmos resultados bit-exatos entre instâncias. A oclusão é um INPUT do
// chamador ([0,1] — o raycast voxel vive no mundo, não aqui), mantendo o
// contrato desacoplado. JSON versionado all-or-nothing bit-exact.
//
// Modelo:
//   - listener: posição + forward (direção de escuta); a projeção horizontal
//     (plano XZ) define o azimute de cada fonte → panning equal-power
//     (L²+R²=1, pan = θ/π com θ = atan2(sin,cos) ∈ [-π,π]).
//   - fonte 3D: posição + gain_db + prioridade [0,1] + oclusão [0,1];
//     attenu = rolloff(distância) ∈ [0,1]; ganho total =
//     db_to_linear(master_gain_db + gain_db) * attenu * (1 - oclusão),
//     dividido em L/R pelo pan.
//   - fonte ambiente (is_3d=false): sem atenuação/pan/virtualização.
//   - reverb zone (AABB, half_extents > 0): wet = zone.wet * cobertura
//     (1 no centro → 0 na borda, linear por eixo, min das 3); a fonte usa a
//     zona mais forte (max).
//   - virtualização: budget max_voices (>= 1); as fontes 3D além do budget
//     são virtualizadas em ordem determinística (prioridade DESC, id ASC) —
//     resultado zero, mas o estado da fonte permanece.

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace engine::audio {

// Vetor 3D do domínio de áudio (self-contained; zero → zero, nunca NaN).
struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    double length_sq() const { return x * x + y * y + z * z; }
    double length() const { return std::sqrt(length_sq()); }

    // Projeção horizontal normalizada (plano XZ). Retorna zero se a projeção
    // for nula (nunca divide por zero).
    Vec3 normalized_xz() const {
        const double len = std::sqrt(x * x + z * z);
        if (len <= 0.0) {
            return {};
        }
        return {x / len, 0.0, z / len};
    }
};

enum class RolloffModel { Linear, Inverse, InverseSquare };

struct AudioReverbZone {
    std::string id;
    Vec3 center;
    Vec3 half_extents;   // AABB: meio-tamanho por eixo (todos > 0)
    double wet = 0.5;    // máximo wet [0,1]
    double decay = 0.5;  // metadado do tail (não usado na matemática)
};

struct AudioSpatialSpec {
    double min_distance = 1.0;   // ganho 1 até aqui
    double max_distance = 100.0; // ganho 0 a partir daqui
    RolloffModel rolloff = RolloffModel::Inverse;
    double master_gain_db = 0.0;
    std::vector<AudioReverbZone> zones;

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& json, std::string& errorOut);
    std::string to_json() const;  // bit-exact (%.9g)
};

struct AudioSourceInput {
    Vec3 position;
    double gain_db = 0.0;    // ganho da fonte (dB)
    double priority = 0.5;   // [0,1] — virtualização
    double occlusion = 0.0;  // [0,1] — input do mundo (raycast voxel)
    bool is_3d = true;       // false = ambiente (sem espacialização)
};

struct AudioSourceResult {
    double gain_l = 0.0;  // ganho linear final (esquerda)
    double gain_r = 0.0;  // ganho linear final (direita)
    double wet = 0.0;     // reverb send [0,1]
    bool virtualized = false;
};

// Espacializador (core de posicionamento/prioridade determinístico).
class ISpatialAudio {
public:
    virtual ~ISpatialAudio() = default;

    // Aplica a spec (all-or-nothing via AudioSpatialSpec::validate).
    virtual bool configure(const AudioSpatialSpec& spec, std::string& errorOut) = 0;

    // Listener: posição + direção de escuta. O forward precisa de projeção
    // horizontal não-nula (o azimute é definido no plano XZ).
    virtual bool set_listener(const Vec3& position, const Vec3& forward,
                              std::string& errorOut) = 0;

    // Define/atualiza uma fonte (all-or-nothing na validação do input).
    virtual bool set_source(const std::string& id, const AudioSourceInput& input,
                            std::string& errorOut) = 0;

    virtual bool remove_source(const std::string& id, std::string& errorOut) = 0;

    // Recomputa os resultados de todas as fontes (determinístico; iteração
    // sorted por id). Deve ser chamado uma vez por frame.
    virtual bool update(std::string& errorOut) = 0;

    // Resultado espacial de uma fonte (zeros para id desconhecido).
    virtual AudioSourceResult source_result(const std::string& id) const = 0;
    virtual bool source_active(const std::string& id) const = 0;

    // Budget de vozes: fontes 3D além do budget são virtualizadas.
    virtual bool set_max_voices(int maxVoices, std::string& errorOut) = 0;

    // Fontes 3D virtualizadas no último update (ordem determinística:
    // prioridade DESC, id ASC).
    virtual std::vector<std::string> virtualized_sources() const = 0;

    // Estado (listener + fontes + budget) serializado bit-exact / restaurado
    // all-or-nothing.
    virtual std::string serialize_state() const = 0;
    virtual bool deserialize_state(const std::string& json,
                                   std::string& errorOut) = 0;
};

// Fábrica do adapter (o único TU implementando ISpatialAudio).
std::unique_ptr<ISpatialAudio> create_spatial_audio();

}  // namespace engine::audio
