#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Engine::Audio {

struct DecodedAudio {
    uint32_t sampleRate{48000};
    uint32_t channels{1};
    std::vector<float> samples; // interleaved PCM [-1, 1]
    std::string error;

    [[nodiscard]] bool valid() const noexcept { return sampleRate > 0 && channels > 0 && !samples.empty(); }
};

// Decodes OGG Vorbis into interleaved float PCM using miniaudio.
class OggDecoder final {
public:
    static std::optional<DecodedAudio> decode_file(const std::filesystem::path& path);
    static std::optional<DecodedAudio> decode_bytes(std::span<const uint8_t> bytes);
};

} // namespace Engine::Audio
