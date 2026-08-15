#include "engine/audio/AudioRuntime.hpp"
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace Engine::Audio {

std::size_t AudioBuffer::frame_count() const noexcept {
    return samples.size() / channels;
}
float AudioBuffer::duration_seconds() const noexcept {
    return static_cast<float>(frame_count()) / static_cast<float>(sampleRate);
}
bool AudioBuffer::valid() const noexcept {
    return !samples.empty() && channels > 0 && sampleRate > 0;
}

std::vector<WaveformPeak> Waveform::build(const AudioBuffer& buffer, std::size_t bucketCount) {
    if (bucketCount == 0 || !buffer.valid()) return {};
    std::vector<WaveformPeak> peaks(bucketCount);
    std::size_t framesPerBucket = buffer.frame_count() / bucketCount;
    if (framesPerBucket == 0) framesPerBucket = 1;

    for (std::size_t i = 0; i < bucketCount; ++i) {
        std::size_t start = i * framesPerBucket * buffer.channels;
        std::size_t end = std::min(start + framesPerBucket * buffer.channels, buffer.samples.size());
        
        float minVal = 0.0f;
        float maxVal = 0.0f;
        float sumSq = 0.0f;
        
        for (std::size_t j = start; j < end; ++j) {
            float s = buffer.samples[j];
            minVal = std::min(minVal, s);
            maxVal = std::max(maxVal, s);
            sumSq += s * s;
        }
        
        peaks[i].minimum = minVal;
        peaks[i].maximum = maxVal;
        peaks[i].rms = std::sqrt(sumSq / static_cast<float>(std::max<std::size_t>(1, end - start)));
    }
    return peaks;
}

AudioClip::AudioClip(std::string name) : name_(std::move(name)) {}

std::shared_ptr<const AudioBuffer> AudioClip::snapshot() const noexcept {
    return std::atomic_load(&data_);
}

std::uint64_t AudioClip::version() const noexcept {
    return version_.load(std::memory_order_acquire);
}

bool AudioClip::hot_swap(AudioBuffer replacement) {
    std::shared_ptr<const AudioBuffer> newBuf = std::make_shared<AudioBuffer>(std::move(replacement));
    std::atomic_store(&data_, newBuf);
    version_.fetch_add(1, std::memory_order_release);
    return true;
}

std::vector<WaveformPeak> AudioClip::waveform(std::size_t bucketCount) const {
    auto snap = snapshot();
    if (!snap) return {};
    return Waveform::build(*snap, bucketCount);
}

StreamingBuffer::StreamingBuffer(std::uint32_t channels, std::size_t capacityFrames) 
    : channels_(channels), capacityFrames_(capacityFrames) {
    ring_.resize(capacityFrames * channels);
}

std::size_t StreamingBuffer::available_frames() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return sizeFrames_;
}

std::size_t StreamingBuffer::writable_frames() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return capacityFrames_ - sizeFrames_;
}

std::size_t StreamingBuffer::push(std::span<const float> interleaved) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t framesToPush = interleaved.size() / channels_;
    std::size_t pushed = 0;
    while (pushed < framesToPush && sizeFrames_ < capacityFrames_) {
        ring_[writeFrame_ * channels_] = interleaved[pushed * channels_];
        if (channels_ > 1) ring_[writeFrame_ * channels_ + 1] = interleaved[pushed * channels_ + 1];
        writeFrame_ = (writeFrame_ + 1) % capacityFrames_;
        sizeFrames_++;
        pushed++;
    }
    return pushed;
}

std::size_t StreamingBuffer::pop(std::span<float> destination, bool zeroFillUnderrun) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t framesToPop = destination.size() / channels_;
    std::size_t popped = 0;
    while (popped < framesToPop && sizeFrames_ > 0) {
        destination[popped * channels_] = ring_[readFrame_ * channels_];
        if (channels_ > 1) destination[popped * channels_ + 1] = ring_[readFrame_ * channels_ + 1];
        readFrame_ = (readFrame_ + 1) % capacityFrames_;
        sizeFrames_--;
        popped++;
    }
    if (zeroFillUnderrun && popped < framesToPop) {
        std::fill(destination.begin() + popped * channels_, destination.end(), 0.0f);
    }
    return popped;
}

void StreamingBuffer::clear() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    readFrame_ = 0;
    writeFrame_ = 0;
    sizeFrames_ = 0;
}

void StreamingBuffer::mark_end_of_stream(bool ended) noexcept {
    endOfStream_.store(ended);
}

bool StreamingBuffer::end_of_stream() const noexcept {
    return endOfStream_.load();
}

AttenuationCurve::AttenuationCurve(AttenuationModel model) : model_(model) {}

void AttenuationCurve::set_rolloff(float rolloff) noexcept {
    rolloff_ = std::max(0.001f, rolloff);
}

void AttenuationCurve::set_points(std::vector<AttenuationPoint> points) {
    points_ = std::move(points);
}

float AttenuationCurve::evaluate(float distance, float minimumDistance, float maximumDistance) const noexcept {
    if (distance <= minimumDistance) return 1.0f;
    if (distance >= maximumDistance) return 0.0f;
    
    switch (model_) {
        case AttenuationModel::Linear:
            return 1.0f - (distance - minimumDistance) / (maximumDistance - minimumDistance);
        case AttenuationModel::Inverse:
            return (minimumDistance / (minimumDistance + rolloff_ * (distance - minimumDistance)));
        case AttenuationModel::Exponential:
            return std::pow(minimumDistance / distance, rolloff_);
        case AttenuationModel::Custom:
            // Placeholder for custom curve evaluation
            return 1.0f;
    }
    return 1.0f;
}

void GainEffect::set_gain(float gain) noexcept { gain_ = gain; }
void GainEffect::process(std::span<float> interleaved, std::uint32_t channels, std::uint32_t sampleRate) {
    for (auto& s : interleaved) s *= gain_;
}

void LowPassEffect::set_cutoff(float cutoffHz) noexcept { cutoffHz_ = cutoffHz; }
void LowPassEffect::process(std::span<float> interleaved, std::uint32_t channels, std::uint32_t sampleRate) {
    if (state_.size() != channels) state_.resize(channels, 0.0f);
    float rc = 1.0f / (2.0f * 3.14159f * cutoffHz_);
    float dt = 1.0f / sampleRate;
    float alpha = dt / (rc + dt);
    
    for (std::size_t i = 0; i < interleaved.size(); i += channels) {
        for (std::uint32_t c = 0; c < channels; ++c) {
            state_[c] = state_[c] + alpha * (interleaved[i + c] - state_[c]);
            interleaved[i + c] = state_[c];
        }
    }
}
void LowPassEffect::reset() noexcept { std::fill(state_.begin(), state_.end(), 0.0f); }

DelayEffect::DelayEffect(float delaySeconds, float feedback, float wet) {
    configure(delaySeconds, feedback, wet);
}
void DelayEffect::configure(float delaySeconds, float feedback, float wet) noexcept {
    delaySeconds_ = delaySeconds;
    feedback_ = feedback;
    wet_ = wet;
}
void DelayEffect::process(std::span<float> interleaved, std::uint32_t channels, std::uint32_t sampleRate) {
    if (configuredRate_ != sampleRate || configuredChannels_ != channels) {
        configuredRate_ = sampleRate;
        configuredChannels_ = channels;
        line_.assign(static_cast<std::size_t>(sampleRate * delaySeconds_ * channels), 0.0f);
        cursor_ = 0;
    }
    if (line_.empty()) return;
    
    for (std::size_t i = 0; i < interleaved.size(); i += channels) {
        for (std::uint32_t c = 0; c < channels; ++c) {
            float in = interleaved[i + c];
            float out = line_[cursor_ + c];
            line_[cursor_ + c] = in + out * feedback_;
            interleaved[i + c] = in * (1.0f - wet_) + out * wet_;
        }
        cursor_ = (cursor_ + channels) % line_.size();
    }
}
void DelayEffect::reset() noexcept { std::fill(line_.begin(), line_.end(), 0.0f); cursor_ = 0; }

Mixer::Mixer(std::uint32_t sampleRate, std::uint32_t outputChannels) 
    : sampleRate_(sampleRate), outputChannels_(outputChannels) {
    buses_[masterBus_] = Bus{{ "Master", InvalidBus, 1.0f }};
}

BusId Mixer::create_bus(BusDescription description) {
    std::lock_guard<std::mutex> lock(mutex_);
    BusId id = nextBus_++;
    buses_[id] = Bus{ std::move(description) };
    return id;
}

bool Mixer::remove_bus(BusId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id == masterBus_) return false;
    return buses_.erase(id) > 0;
}

bool Mixer::set_bus_gain(BusId id, float gain) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = buses_.find(id); it != buses_.end()) {
        it->second.description.gain = gain;
        return true;
    }
    return false;
}

bool Mixer::set_bus_muted(BusId id, bool muted) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = buses_.find(id); it != buses_.end()) {
        it->second.muted = muted;
        return true;
    }
    return false;
}

bool Mixer::add_effect(BusId id, std::shared_ptr<AudioEffect> effect) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = buses_.find(id); it != buses_.end()) {
        it->second.effects.push_back(std::move(effect));
        return true;
    }
    return false;
}

bool Mixer::clear_effects(BusId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = buses_.find(id); it != buses_.end()) {
        it->second.effects.clear();
        return true;
    }
    return false;
}

VoiceId Mixer::play(VoiceDescription description) {
    std::lock_guard<std::mutex> lock(mutex_);
    VoiceId id = nextVoice_++;
    Voice v;
    v.description = std::move(description);
    v.active = true;
    voices_[id] = std::move(v);
    return id;
}

bool Mixer::stop(VoiceId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return voices_.erase(id) > 0;
}

bool Mixer::pause(VoiceId id, bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = voices_.find(id); it != voices_.end()) {
        it->second.paused = paused;
        return true;
    }
    return false;
}

bool Mixer::set_voice_gain(VoiceId id, float gain) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = voices_.find(id); it != voices_.end()) {
        it->second.description.gain = gain;
        return true;
    }
    return false;
}

bool Mixer::set_voice_position(VoiceId id, const glm::vec3& position) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = voices_.find(id); it != voices_.end()) {
        it->second.description.position = position;
        return true;
    }
    return false;
}

bool Mixer::set_voice_occlusion(VoiceId id, float amount) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = voices_.find(id); it != voices_.end()) {
        it->second.description.occlusion = amount;
        return true;
    }
    return false;
}

bool Mixer::set_environment_send(VoiceId id, BusId destination, float gain) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = voices_.find(id); it != voices_.end()) {
        it->second.description.environmentSends[destination] = gain;
        return true;
    }
    return false;
}

void Mixer::set_listener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) {
    std::lock_guard<std::mutex> lock(mutex_);
    listenerPosition_ = position;
    listenerForward_ = forward;
    listenerUp_ = up;
}

std::span<const float> Mixer::render(std::size_t frameCount) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t sampleCount = frameCount * outputChannels_;
    output_.assign(sampleCount, 0.0f);
    
    // Clear buses
    for (auto& [id, bus] : buses_) {
        bus.buffer.assign(sampleCount, 0.0f);
    }

    // Render voices
    for (auto it = voices_.begin(); it != voices_.end();) {
        if (!it->second.active) {
            it = voices_.erase(it);
            continue;
        }
        
        if (!it->second.paused) {
            BusId busId = it->second.description.bus;
            if (buses_.find(busId) == buses_.end()) busId = masterBus_;
            render_voice(it->second, std::span<float>(buses_[busId].buffer), frameCount);
        }
        ++it;
    }
    
    // Mix buses to master (simplified topology)
    for (auto& [id, bus] : buses_) {
        if (id == masterBus_) continue;
        if (bus.muted) continue;
        
        for (auto& effect : bus.effects) {
            effect->process(std::span<float>(bus.buffer), outputChannels_, sampleRate_);
        }
        
        BusId parent = bus.description.parent;
        if (buses_.find(parent) == buses_.end()) parent = masterBus_;
        
        for (std::size_t i = 0; i < sampleCount; ++i) {
            buses_[parent].buffer[i] += bus.buffer[i] * bus.description.gain;
        }
    }
    
    // Process master
    auto& master = buses_[masterBus_];
    for (auto& effect : master.effects) {
        effect->process(std::span<float>(master.buffer), outputChannels_, sampleRate_);
    }
    
    for (std::size_t i = 0; i < sampleCount; ++i) {
        output_[i] = master.buffer[i] * master.description.gain;
    }
    
    return output_;
}

std::size_t Mixer::active_voice_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return voices_.size();
}

bool Mixer::is_active(VoiceId id) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = voices_.find(id);
    return it != voices_.end() && it->second.active;
}

void Mixer::render_voice(Voice& voice, std::span<float> destination, std::size_t frameCount) {
    if (voice.description.clip) {
        auto snap = voice.description.clip->snapshot();
        if (!snap || snap->samples.empty()) return;
        
        std::size_t srcChannels = snap->channels;
        for (std::size_t i = 0; i < frameCount; ++i) {
            std::size_t idx = static_cast<std::size_t>(voice.cursor);
            if (idx >= snap->frame_count()) {
                if (voice.description.looping) {
                    voice.cursor = 0;
                    idx = 0;
                } else {
                    voice.active = false;
                    break;
                }
            }
            
            float sampleL = snap->samples[idx * srcChannels];
            float sampleR = srcChannels > 1 ? snap->samples[idx * srcChannels + 1] : sampleL;
            
            float distanceGain = 1.0f;
            if (voice.description.spatial) {
                float dist = glm::distance(listenerPosition_, voice.description.position);
                distanceGain = voice.description.attenuation.evaluate(dist, voice.description.minimumDistance, voice.description.maximumDistance);
                // Simple panning based on dot product
                glm::vec3 dir = glm::normalize(voice.description.position - listenerPosition_);
                glm::vec3 right = glm::cross(listenerForward_, listenerUp_);
                float pan = glm::dot(dir, right);
                sampleL *= std::min(1.0f, 1.0f - pan);
                sampleR *= std::min(1.0f, 1.0f + pan);
            }
            
            float totalGain = voice.description.gain * distanceGain;
            destination[i * outputChannels_] += sampleL * totalGain;
            if (outputChannels_ > 1) {
                destination[i * outputChannels_ + 1] += sampleR * totalGain;
            }
            
            voice.cursor += voice.description.pitch;
        }
    }
}

bool Mixer::bus_exists(BusId id) const noexcept {
    return buses_.find(id) != buses_.end();
}

bool Mixer::would_cycle(BusId child, BusId parent) const noexcept {
    BusId current = parent;
    while (current != InvalidBus) {
        if (current == child) return true;
        if (buses_.find(current) == buses_.end()) break;
        current = buses_.at(current).description.parent;
    }
    return false;
}

} // namespace Engine::Audio
