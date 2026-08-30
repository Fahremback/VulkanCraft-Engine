#pragma once

#include <glm/glm.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Engine::Audio {

using BusId = std::uint32_t;
using VoiceId = std::uint64_t;
inline constexpr BusId InvalidBus = 0;

struct AudioBuffer {
    std::uint32_t sampleRate{48000};
    std::uint32_t channels{1};
    std::vector<float> samples;

    [[nodiscard]] std::size_t frame_count() const noexcept;
    [[nodiscard]] float duration_seconds() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
};

struct WaveformPeak {
    float minimum{};
    float maximum{};
    float rms{};
};

class Waveform final {
public:
    static std::vector<WaveformPeak> build(const AudioBuffer& buffer, std::size_t bucketCount);
};

// A clip owns an immutable PCM snapshot. Readers keep their snapshot alive while a
// producer atomically replaces it, making editor/importer hot-reload click-safe.
class AudioClip final {
public:
    explicit AudioClip(std::string name = {});

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] std::shared_ptr<const AudioBuffer> snapshot() const noexcept;
    [[nodiscard]] std::uint64_t version() const noexcept;
    bool hot_swap(AudioBuffer replacement);
    [[nodiscard]] std::vector<WaveformPeak> waveform(std::size_t bucketCount) const;

private:
    std::string name_;
    std::shared_ptr<const AudioBuffer> data_;
    std::atomic<std::uint64_t> version_{0};
};

// Bounded interleaved PCM ring. A decoder/IO thread pushes complete frames while
// the realtime thread pops them. Overflow is rejected; underruns are zero-filled.
class StreamingBuffer final {
public:
    StreamingBuffer(std::uint32_t channels, std::size_t capacityFrames);

    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }
    [[nodiscard]] std::size_t capacity_frames() const noexcept { return capacityFrames_; }
    [[nodiscard]] std::size_t available_frames() const noexcept;
    [[nodiscard]] std::size_t writable_frames() const noexcept;
    std::size_t push(std::span<const float> interleaved);
    std::size_t pop(std::span<float> destination, bool zeroFillUnderrun = true);
    void clear() noexcept;
    void mark_end_of_stream(bool ended = true) noexcept;
    [[nodiscard]] bool end_of_stream() const noexcept;

private:
    std::uint32_t channels_;
    std::size_t capacityFrames_;
    std::vector<float> ring_;
    std::size_t readFrame_{};
    std::size_t writeFrame_{};
    std::size_t sizeFrames_{};
    mutable std::mutex mutex_;
    std::atomic<bool> endOfStream_{false};
};

enum class AttenuationModel { Linear, Inverse, Exponential, Custom };

struct AttenuationPoint {
    float normalizedDistance{};
    float gain{1.0f};
};

class AttenuationCurve final {
public:
    explicit AttenuationCurve(AttenuationModel model = AttenuationModel::Inverse);
    void set_model(AttenuationModel model) noexcept { model_ = model; }
    void set_rolloff(float rolloff) noexcept;
    void set_points(std::vector<AttenuationPoint> points);
    [[nodiscard]] float evaluate(float distance, float minimumDistance, float maximumDistance) const noexcept;

private:
    AttenuationModel model_;
    float rolloff_{1.0f};
    std::vector<AttenuationPoint> points_;
};

class AudioEffect {
public:
    virtual ~AudioEffect() = default;
    virtual void process(std::span<float> interleaved, std::uint32_t channels,
                         std::uint32_t sampleRate) = 0;
    virtual void reset() noexcept {}
};

class GainEffect final : public AudioEffect {
public:
    explicit GainEffect(float gain = 1.0f) : gain_(gain) {}
    void set_gain(float gain) noexcept;
    [[nodiscard]] float gain() const noexcept { return gain_; }
    void process(std::span<float> interleaved, std::uint32_t channels,
                 std::uint32_t sampleRate) override;
private:
    float gain_;
};

class LowPassEffect final : public AudioEffect {
public:
    explicit LowPassEffect(float cutoffHz = 18000.0f) : cutoffHz_(cutoffHz) {}
    void set_cutoff(float cutoffHz) noexcept;
    void process(std::span<float> interleaved, std::uint32_t channels,
                 std::uint32_t sampleRate) override;
    void reset() noexcept override;
private:
    float cutoffHz_;
    std::vector<float> state_;
};

class DelayEffect final : public AudioEffect {
public:
    DelayEffect(float delaySeconds = 0.18f, float feedback = 0.25f, float wet = 0.2f);
    void configure(float delaySeconds, float feedback, float wet) noexcept;
    void process(std::span<float> interleaved, std::uint32_t channels,
                 std::uint32_t sampleRate) override;
    void reset() noexcept override;
private:
    float delaySeconds_;
    float feedback_;
    float wet_;
    std::uint32_t configuredRate_{};
    std::uint32_t configuredChannels_{};
    std::vector<float> line_;
    std::size_t cursor_{};
};

struct BusDescription {
    std::string name;
    BusId parent{InvalidBus};
    float gain{1.0f};
};

struct VoiceDescription {
    std::shared_ptr<AudioClip> clip;
    std::shared_ptr<StreamingBuffer> stream;
    BusId bus{InvalidBus};
    float gain{1.0f};
    float pitch{1.0f};
    bool looping{};
    bool spatial{};
    glm::vec3 position{0.0f};
    float minimumDistance{1.0f};
    float maximumDistance{100.0f};
    AttenuationCurve attenuation;
    float occlusion{};
    std::unordered_map<BusId, float> environmentSends;
};

class Mixer final {
public:
    explicit Mixer(std::uint32_t sampleRate = 48000, std::uint32_t outputChannels = 2);

    [[nodiscard]] BusId master_bus() const noexcept { return masterBus_; }
    [[nodiscard]] BusId create_bus(BusDescription description);
    bool remove_bus(BusId id);
    bool set_bus_gain(BusId id, float gain);
    bool set_bus_muted(BusId id, bool muted);
    bool add_effect(BusId id, std::shared_ptr<AudioEffect> effect);
    bool clear_effects(BusId id);

    [[nodiscard]] VoiceId play(VoiceDescription description);
    bool stop(VoiceId id);
    bool pause(VoiceId id, bool paused);
    bool set_voice_gain(VoiceId id, float gain);
    bool set_voice_position(VoiceId id, const glm::vec3& position);
    bool set_voice_occlusion(VoiceId id, float amount);
    bool set_environment_send(VoiceId id, BusId destination, float gain);
    void set_listener(const glm::vec3& position, const glm::vec3& forward,
                      const glm::vec3& up = glm::vec3(0.0f, 1.0f, 0.0f));

    // Produces exactly frameCount interleaved samples. The returned span remains
    // valid until the next render call.
    [[nodiscard]] std::span<const float> render(std::size_t frameCount);
    // True while the voice is still playing (rendered, not finished/stopped).
    [[nodiscard]] bool is_active(VoiceId id) const noexcept;
    [[nodiscard]] std::size_t active_voice_count() const noexcept;
    // Live RMS level of the voice's last rendered block (0 when the voice is
    // inactive/absent). Computed under the same mutex as render(), so it can be
    // read from the game/editor thread to drive audio-linked effects.
    [[nodiscard]] float voice_level(VoiceId id) const noexcept;

private:
    struct Bus {
        BusDescription description;
        bool muted{};
        std::vector<std::shared_ptr<AudioEffect>> effects;
        std::vector<float> buffer;
    };

    struct Voice {
        VoiceDescription description;
        double cursor{};
        std::uint64_t observedVersion{};
        std::size_t observedFrames{};
        bool paused{};
        bool active{true};
        // Live RMS level of the last rendered block (computed in render_voice
        // under the mixer mutex). Drives data-driven effects (e.g. particles
        // pulsing with audio) — see voice_level().
        float level{0.0f};
        std::vector<float> occlusionState;
    };

    [[nodiscard]] bool bus_exists(BusId id) const noexcept;
    [[nodiscard]] bool would_cycle(BusId child, BusId parent) const noexcept;
    void render_voice(Voice& voice, std::span<float> destination, std::size_t frameCount);

    std::uint32_t sampleRate_;
    std::uint32_t outputChannels_;
    BusId masterBus_{1};
    BusId nextBus_{2};
    VoiceId nextVoice_{1};
    glm::vec3 listenerPosition_{0.0f};
    glm::vec3 listenerForward_{0.0f, 0.0f, -1.0f};
    glm::vec3 listenerUp_{0.0f, 1.0f, 0.0f};
    std::unordered_map<BusId, Bus> buses_;
    std::unordered_map<VoiceId, Voice> voices_;
    std::vector<float> output_;
    mutable std::mutex mutex_;
};

} // namespace Engine::Audio
