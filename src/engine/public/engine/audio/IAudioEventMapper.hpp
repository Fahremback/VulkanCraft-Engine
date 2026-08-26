// IAudioEventMapper — mapeia eventos de gameplay para TRIGGERS de áudio.
// Componente CORE do §7 item 76 ("integrar materiais, impactos, passos,
// clima, veículos, abilities, portais e destruição por eventos públicos"):
// o runtime emite eventos (IGameplayEvents #253, kinds opacos); o mapper
// traduz cada kind de evento numa descrição de som (soundId + volume + pitch)
// que o motor de áudio (IAudioEvent/ISpatialAudio) executa. Puro e
// determinístico (triggers em ordem crescente de eventKind); a reprodução
// real fica no motor de áudio.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace audio {

struct AudioTrigger {
    std::string eventKind;   // ex.: "explosion", "footstep_stone"
    std::string soundId;     // asset de som (não-vazio)
    float volume{ 1.0f };    // [0,1]
    float pitch{ 1.0f };     // > 0
};

class IAudioEventMapper {
public:
    virtual ~IAudioEventMapper() = default;

    // All-or-nothing: eventKind vazio/duplicado, soundId vazio, volume fora
    // de [0,1], pitch <= 0 ou não-finitos → rejeita a lista inteira.
    virtual bool configure(const std::vector<AudioTrigger>& triggers,
                           std::string& errorOut) = 0;

    virtual const AudioTrigger* trigger_for(const std::string& eventKind) const = 0;
    virtual std::vector<AudioTrigger> triggers() const = 0;  // ordem por eventKind
    virtual std::size_t count() const = 0;
    virtual void clear() = 0;
};

std::unique_ptr<IAudioEventMapper> create_audio_event_mapper();

}  // namespace audio
}  // namespace engine
