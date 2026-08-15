#include "AudioZones.hpp"

#include <algorithm>
#include <cmath>

namespace Engine::Audio {

float ReverbZone::coverage(const glm::vec3& point) const noexcept {
    const glm::vec3 delta = point - center;
    const glm::vec3 absDelta(std::abs(delta.x), std::abs(delta.y), std::abs(delta.z));
    // Distance outside the box along the dominant axis, in units of 1 cell.
    float outside = 0.0f;
    outside = std::max(outside, absDelta.x - halfExtents.x);
    outside = std::max(outside, absDelta.y - halfExtents.y);
    outside = std::max(outside, absDelta.z - halfExtents.z);
    if (outside <= 0.0f) return 1.0f;
    // Soft falloff over one "cell" (approximated as the smallest extent).
    const float falloff = std::max(1.0f, std::min({halfExtents.x, halfExtents.y, halfExtents.z}));
    return std::max(0.0f, 1.0f - outside / falloff);
}

std::optional<float> ReverbZoneSystem::wet_at(const glm::vec3& listener) const {
    float best = 0.0f;
    bool any = false;
    for (const ReverbZone& zone : zones_) {
        const float coverage = zone.coverage(listener);
        if (coverage > 0.0f) {
            any = true;
            best = std::max(best, zone.wet * coverage);
        }
    }
    if (!any) return std::nullopt;
    return best;
}

void ReverbZoneSystem::add_zone(ReverbZone zone) {
    zones_.push_back(std::move(zone));
}

bool ReverbZoneSystem::remove_zone(const std::string& name) {
    const auto it = std::find_if(zones_.begin(), zones_.end(),
                                 [&](const ReverbZone& z) { return z.name == name; });
    if (it == zones_.end()) return false;
    zones_.erase(it);
    return true;
}

void ReverbZoneSystem::clear() noexcept {
    zones_.clear();
}

AmbientZoneSystem::AmbientZoneSystem(Mixer& mixer) : mixer_(mixer) {}

void AmbientZoneSystem::add_zone(AmbientZone zone) {
    zones_.push_back(std::move(zone));
    zoneVoices_.push_back(std::nullopt);
}

bool AmbientZoneSystem::remove_zone(const std::string& name) {
    for (size_t i = 0; i < zones_.size(); ++i) {
        if (zones_[i].name != name) continue;
        if (zoneVoices_[i]) mixer_.stop(*zoneVoices_[i]);
        zones_.erase(zones_.begin() + static_cast<std::ptrdiff_t>(i));
        zoneVoices_.erase(zoneVoices_.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    return false;
}

void AmbientZoneSystem::clear() {
    for (auto& voice : zoneVoices_) {
        if (voice) mixer_.stop(*voice);
    }
    zones_.clear();
    zoneVoices_.clear();
    activeZones_ = 0;
}

void AmbientZoneSystem::update(const glm::vec3& listenerPosition) {
    activeZones_ = 0;
    for (size_t i = 0; i < zones_.size(); ++i) {
        AmbientZone& zone = zones_[i];
        const glm::vec3 delta = listenerPosition - zone.center;
        const glm::vec3 absDelta(std::abs(delta.x), std::abs(delta.y), std::abs(delta.z));
        const bool inside = absDelta.x <= zone.halfExtents.x &&
                            absDelta.y <= zone.halfExtents.y &&
                            absDelta.z <= zone.halfExtents.z;
        if (inside && zone.clip) {
            if (!zoneVoices_[i]) {
                VoiceDescription desc;
                desc.clip = zone.clip;
                desc.looping = true;
                desc.gain = zone.gain;
                zoneVoices_[i] = mixer_.play(desc);
            } else {
                mixer_.set_voice_gain(*zoneVoices_[i], zone.gain);
            }
            ++activeZones_;
        } else if (zoneVoices_[i]) {
            mixer_.stop(*zoneVoices_[i]);
            zoneVoices_[i].reset();
        }
    }
}

} // namespace Engine::Audio
