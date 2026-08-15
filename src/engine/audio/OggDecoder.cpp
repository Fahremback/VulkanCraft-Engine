#include "OggDecoder.hpp"

#include <miniaudio.h>

#include <vector>

namespace Engine::Audio {

namespace {
std::optional<DecodedAudio> decode_with_decoder(ma_decoder* decoder) {
    DecodedAudio out;

    ma_uint64 totalFrames = 0;
    ma_decoder_get_length_in_pcm_frames(decoder, &totalFrames);

    const ma_uint32 channels = decoder->outputChannels;
    const ma_uint32 sampleRate = decoder->outputSampleRate;
    if (channels == 0 || sampleRate == 0) {
        out.error = "Decoder produced no channel/rate info";
        return out;
    }
    out.channels = channels;
    out.sampleRate = sampleRate;

    const ma_uint64 capacityFrames = (totalFrames > 0) ? totalFrames : (1ull << 20);
    out.samples.reserve(static_cast<size_t>(capacityFrames) * channels);

    std::vector<float> chunk(4096 * channels);
    for (;;) {
        ma_uint64 framesRead = 0;
        const ma_result readResult = ma_decoder_read_pcm_frames(decoder, chunk.data(), 4096, &framesRead);
        if (readResult != MA_SUCCESS || framesRead == 0) break;
        out.samples.insert(out.samples.end(), chunk.begin(),
                           chunk.begin() + static_cast<std::ptrdiff_t>(framesRead * channels));
        if (framesRead < 4096) break;
    }
    if (out.samples.empty()) {
        out.error = "Decoded stream contained no PCM frames";
        return out;
    }
    return out;
}
} // namespace

std::optional<DecodedAudio> OggDecoder::decode_file(const std::filesystem::path& path) {
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder;
    if (ma_decoder_init_file(path.string().c_str(), &config, &decoder) != MA_SUCCESS) {
        DecodedAudio out;
        out.error = "miniaudio could not open " + path.string();
        return out;
    }
    auto result = decode_with_decoder(&decoder);
    ma_decoder_uninit(&decoder);
    return result;
}

std::optional<DecodedAudio> OggDecoder::decode_bytes(std::span<const uint8_t> bytes) {
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder;
    if (ma_decoder_init_memory(bytes.data(), bytes.size(), &config, &decoder) != MA_SUCCESS) {
        DecodedAudio out;
        out.error = "miniaudio could not open memory stream";
        return out;
    }
    auto result = decode_with_decoder(&decoder);
    ma_decoder_uninit(&decoder);
    return result;
}

} // namespace Engine::Audio
