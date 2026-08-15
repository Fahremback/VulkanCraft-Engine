#pragma once
#include "AudioRuntime.hpp"
#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Engine::Audio {

// A 3D reverb zone: a box/capsule volume that applies a wet reverb send to
// voices inside it, with a smooth falloff at the boundary.
struct ReverbZone {
    std::string name{"ReverbZone"};
    glm::vec3 center{0.0f};
    glm::vec3 halfExtents{5.0f};
    float wet{0.3f};        // max send gain to the reverb bus
    float decay{0.35f};     // reverb tail decay
    float preDelay{0.02f};  // pre-delay seconds
    float mixSmoothing{4.0f}; // how fast sends fade when crossing the boundary

    // 1 inside, 0 outside (with a soft 1-cell falloff).
    [[nodiscard]] float coverage(const glm::vec3& point) const noexcept;
};

// An ambient zone: a volume inside which an ambient clip plays (looping) with
// volume fading by coverage. Managed by AmbientZoneSystem.
struct AmbientZone {
    std::string name{"AmbientZone"};
    glm::vec3 center{0.0f};
    glm::vec3 halfExtents{10.0f};
    std::shared_ptr<AudioClip> clip;
    float gain{0.7f};
};

class ReverbZoneSystem final {
public:
    // Returns the strongest reverb wet value at the listener position, or nullopt.
    [[nodiscard]] std::optional<float> wet_at(const glm::vec3& listener) const;

    void add_zone(ReverbZone zone);
    bool remove_zone(const std::string& name);
    void clear() noexcept;
    [[nodiscard]] const std::vector<ReverbZone>& zones() const noexcept { return zones_; }

private:
    std::vector<ReverbZone> zones_;
};

class AmbientZoneSystem final {
public:
    explicit AmbientZoneSystem(Mixer& mixer);
    void add_zone(AmbientZone zone);
    bool remove_zone(const std::string& name);
    void clear();
    // Call each frame with the listener position; starts/stops/stages zone
    // voices so the ambient is audible only inside its volume.
    void update(const glm::vec3& listenerPosition);
    [[nodiscard]] const std::vector<AmbientZone>& zones() const noexcept { return zones_; }
    [[nodiscard]] std::size_t active_zone_count() const noexcept { return activeZones_; }

private:
    Mixer& mixer_;
    std::vector<AmbientZone> zones_;
    std::vector<std::optional<VoiceId>> zoneVoices_;
    std::size_t activeZones_{0};
};

} // namespace Engine::Audio
