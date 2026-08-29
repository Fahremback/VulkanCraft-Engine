// IAudioCodec — codec de voz/áudio de rede (backend: libopus promovida,
// §8 opus). Superfície pública self-contained: nenhum header do Opus aparece
// aqui; o adapter (src/engine/sdk/OpusCodec.cpp) é o ÚNICO TU que inclui
// <opus.h>. O codec entra no pipeline de rede (Agente 3/5 networking), não no
// núcleo headless — voz de jogador/servidor é um fluxo de bytes opaco para o
// gameplay. Determinístico: dois codecs novos com a mesma configuração e a
// mesma entrada PCM produzem o MESMO pacote (bit-exact).

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace audio {

struct AudioCodecConfig {
    int sampleRate{ 48000 };   // 8k/12k/16k/24k/48k (taxas do Opus)
    int channels{ 1 };         // 1 (mono) ou 2 (stereo)
    int frameSamples{ 960 };   // amostras POR CANAL por quadro (20 ms @ 48 kHz)
    int bitrate{ 32000 };      // bps, [500, 512000]
};

class IAudioCodec {
public:
    virtual ~IAudioCodec() = default;

    virtual int sample_rate() const = 0;
    virtual int channels() const = 0;
    virtual int frame_samples() const = 0;  // amostras por canal por quadro
    virtual int bitrate() const = 0;

    // Codifica exatamente frame_samples()*channels() amostras interleaved de
    // float em [-1,1] num pacote. All-or-nothing: pcm nulo/tamanho errado,
    // estado interno inválido → false + errorOut, pacote intocado.
    virtual bool encode_frame(const float* pcm,
                              std::vector<std::uint8_t>& packet,
                              std::string& errorOut) = 0;

    // Decodifica um pacote em frame_samples()*channels() amostras float.
    // All-or-nothing: pacote malformado/corrompido → false + errorOut.
    virtual bool decode_frame(const std::uint8_t* packet,
                              std::size_t size,
                              std::vector<float>& pcm,
                              std::string& errorOut) = 0;
};

// All-or-nothing na criação: sampleRate fora do conjunto do Opus, channels
// fora de {1,2}, frameSamples fora dos tamanhos suportados pela taxa
// (2.5/5/10/20/40/60 ms), bitrate fora de [500,512000] → nullptr + errorOut.
std::unique_ptr<IAudioCodec> create_opus_codec(const AudioCodecConfig& config,
                                               std::string& errorOut);

}  // namespace audio
}  // namespace engine
