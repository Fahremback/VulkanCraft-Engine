// OpusCodec.cpp — adapter do codec de voz/áudio de rede atrás do contrato
// público engine/audio/IAudioCodec.hpp (§8 opus, DEPENDENCY_POLICY: a
// superfície pública é a nossa; o codec entra no pipeline de rede). ÚNICO TU
// que inclui <opus.h>: aqui vivem o encoder/decoder reais da libopus vendida
// (external/solutions/opus). Determinístico por construção — um encoder novo
// com a mesma config produz pacotes bit-idênticos para a mesma entrada.

#include "engine/audio/IAudioCodec.hpp"

#include <opus.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace engine {
namespace audio {
namespace {

// Tamanhos de quadro aceitos pelo Opus (2.5/5/10/20/40/60 ms por canal).
bool supported_frame_samples(int sampleRate, int frameSamples) {
    const int kMs[] = { 2, 5, 10, 20, 40, 60 };  // 2.5ms arredondado p/ int
    for (int ms : kMs) {
        if (frameSamples == sampleRate * ms / 1000) return true;
    }
    return false;
}

bool supported_sample_rate(int rate) {
    return rate == 8000 || rate == 12000 || rate == 16000 ||
           rate == 24000 || rate == 48000;
}

class OpusCodec final : public IAudioCodec {
public:
    OpusCodec(const AudioCodecConfig& config, ::OpusEncoder* encoder,
              ::OpusDecoder* decoder, int actualBitrate)
        : config_(config), encoder_(encoder), decoder_(decoder),
          actualBitrate_(actualBitrate) {}

    ~OpusCodec() override {
        if (encoder_) ::opus_encoder_destroy(encoder_);
        if (decoder_) ::opus_decoder_destroy(decoder_);
    }

    int sample_rate() const override { return config_.sampleRate; }
    int channels() const override { return config_.channels; }
    int frame_samples() const override { return config_.frameSamples; }
    int bitrate() const override { return actualBitrate_; }

    bool encode_frame(const float* pcm, std::vector<std::uint8_t>& packet,
                      std::string& errorOut) override {
        if (pcm == nullptr) {
            errorOut = "opus codec: pcm null";
            return false;
        }
        packet_.resize(4000);  // máximo seguro de payload Opus
        const int ret = ::opus_encode_float(
            encoder_, pcm, config_.frameSamples, packet_.data(), 4000);
        if (ret < 0) {
            errorOut = std::string("opus codec: encode failed: ") +
                       ::opus_strerror(ret);
            return false;
        }
        packet.assign(packet_.begin(), packet_.begin() + ret);
        return true;
    }

    bool decode_frame(const std::uint8_t* packet, std::size_t size,
                      std::vector<float>& pcm, std::string& errorOut) override {
        if (packet == nullptr && size != 0) {
            errorOut = "opus codec: packet null with size";
            return false;
        }
        decoded_.resize(static_cast<std::size_t>(config_.frameSamples) *
                        static_cast<std::size_t>(config_.channels));
        const int ret = ::opus_decode_float(
            decoder_, packet, static_cast<opus_int32>(size), decoded_.data(),
            config_.frameSamples, 0);
        if (ret < 0) {
            errorOut = std::string("opus codec: decode failed: ") +
                       ::opus_strerror(ret);
            return false;
        }
        pcm = decoded_;
        return true;
    }

private:
    AudioCodecConfig config_;
    ::OpusEncoder* encoder_;
    ::OpusDecoder* decoder_;
    int actualBitrate_;
    std::vector<std::uint8_t> packet_;  // scratch de codificação
    std::vector<float> decoded_;        // scratch de decodificação
};

}  // namespace

std::unique_ptr<IAudioCodec> create_opus_codec(const AudioCodecConfig& config,
                                               std::string& errorOut) {
    if (!supported_sample_rate(config.sampleRate)) {
        errorOut = "opus codec: unsupported sample rate " +
                   std::to_string(config.sampleRate);
        return nullptr;
    }
    if (config.channels != 1 && config.channels != 2) {
        errorOut = "opus codec: channels must be 1 or 2";
        return nullptr;
    }
    if (!supported_frame_samples(config.sampleRate, config.frameSamples)) {
        errorOut = "opus codec: unsupported frameSamples " +
                   std::to_string(config.frameSamples) +
                   " for rate " + std::to_string(config.sampleRate);
        return nullptr;
    }
    if (config.bitrate < 500 || config.bitrate > 512000) {
        errorOut = "opus codec: bitrate out of [500, 512000]";
        return nullptr;
    }

    int err = 0;
    ::OpusEncoder* encoder = ::opus_encoder_create(
        config.sampleRate, config.channels, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || encoder == nullptr) {
        errorOut = std::string("opus codec: encoder create failed: ") +
                   ::opus_strerror(err);
        return nullptr;
    }
    err = ::opus_encoder_ctl(encoder, OPUS_SET_BITRATE(config.bitrate));
    if (err != OPUS_OK) {
        ::opus_encoder_destroy(encoder);
        errorOut = std::string("opus codec: set bitrate failed: ") +
                   ::opus_strerror(err);
        return nullptr;
    }

    ::OpusDecoder* decoder =
        ::opus_decoder_create(config.sampleRate, config.channels, &err);
    if (err != OPUS_OK || decoder == nullptr) {
        ::opus_encoder_destroy(encoder);
        errorOut = std::string("opus codec: decoder create failed: ") +
                   ::opus_strerror(err);
        return nullptr;
    }

    return std::unique_ptr<IAudioCodec>(
        new OpusCodec(config, encoder, decoder, config.bitrate));
}

}  // namespace audio
}  // namespace engine
