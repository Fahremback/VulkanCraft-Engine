#include "ParticleSimulation.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace Engine::Gameplay {
namespace {
glm::vec3 normalized_or(const glm::vec3& value, const glm::vec3& fallback) {
    const float squared = glm::dot(value, value);
    return squared > 1.0e-8f ? value / std::sqrt(squared) : fallback;
}

glm::vec3 turbulence_vector(const glm::vec3& p, float time) {
    return {
        std::sin(p.y * 3.17f + time * 1.31f) * std::cos(p.z * 1.73f - time),
        std::sin(p.z * 2.41f - time * 1.17f) * std::cos(p.x * 2.03f + time),
        std::sin(p.x * 2.89f + time * 0.91f) * std::cos(p.y * 1.57f - time)
    };
}
}

ParticleSimulation::ParticleSimulation(std::size_t capacity, std::uint32_t seed)
    : particles_(capacity), randomState_(seed ? seed : 1u) {
    freeParticles_.reserve(capacity);
    for (std::size_t i = capacity; i > 0; --i) freeParticles_.push_back(i - 1);
}

std::size_t ParticleSimulation::add_emitter(const ParticleEmitterDesc& description) {
    for (std::size_t i = 0; i < emitters_.size(); ++i) if (!emitters_[i].alive) {
        emitters_[i] = {description, 0.0f, true};
        return i;
    }
    emitters_.push_back({description, 0.0f, true});
    return emitters_.size() - 1;
}

bool ParticleSimulation::remove_emitter(std::size_t index) {
    if (index >= emitters_.size() || !emitters_[index].alive) return false;
    emitters_[index].alive = false;
    emitters_[index].accumulator = 0.0f;
    return true;
}

ParticleEmitterDesc* ParticleSimulation::emitter(std::size_t index) {
    return index < emitters_.size() && emitters_[index].alive ? &emitters_[index].description : nullptr;
}

const ParticleEmitterDesc* ParticleSimulation::emitter(std::size_t index) const {
    return index < emitters_.size() && emitters_[index].alive ? &emitters_[index].description : nullptr;
}

float ParticleSimulation::random_unit() {
    randomState_ ^= randomState_ << 13u;
    randomState_ ^= randomState_ >> 17u;
    randomState_ ^= randomState_ << 5u;
    return static_cast<float>(randomState_ & 0x00ffffffu) / static_cast<float>(0x01000000u);
}

glm::vec3 ParticleSimulation::random_direction(const glm::vec3& direction, float coneAngle) {
    const glm::vec3 forward = normalized_or(direction, {0.0f, 1.0f, 0.0f});
    const glm::vec3 reference = std::abs(forward.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 right = normalized_or(glm::cross(reference, forward), {1.0f, 0.0f, 0.0f});
    const glm::vec3 up = glm::cross(forward, right);
    const float cosine = glm::mix(1.0f, std::cos(glm::clamp(coneAngle, 0.0f, glm::pi<float>())), random_unit());
    const float sine = std::sqrt(std::max(0.0f, 1.0f - cosine * cosine));
    const float azimuth = random_unit() * glm::two_pi<float>();
    return forward * cosine + (right * std::cos(azimuth) + up * std::sin(azimuth)) * sine;
}

void ParticleSimulation::spawn(EmitterSlot& slot) {
    if (freeParticles_.empty()) return;
    const std::size_t index = freeParticles_.back();
    freeParticles_.pop_back();
    Particle& particle = particles_[index];
    const auto& emitter = slot.description;
    particle.position = particle.previousPosition = emitter.position;
    particle.velocity = random_direction(emitter.direction, emitter.coneAngle) * glm::mix(emitter.speedMin, emitter.speedMax, random_unit());
    particle.color = emitter.colorStart;
    particle.size = emitter.sizeStart;
    particle.rotation = random_unit() * glm::two_pi<float>();
    particle.angularVelocity = (random_unit() * 2.0f - 1.0f) * 3.0f;
    particle.age = 0.0f;
    particle.lifetime = std::max(0.001f, glm::mix(emitter.lifetimeMin, emitter.lifetimeMax, random_unit()));
    particle.startColor = emitter.colorStart;
    particle.endColor = emitter.colorEnd;
    particle.acceleration = emitter.acceleration;
    particle.startSize = emitter.sizeStart;
    particle.endSize = emitter.sizeEnd;
    particle.drag = emitter.drag;
    particle.turbulence = emitter.turbulence;
    particle.restitution = emitter.restitution;
    particle.collisionLayers = emitter.collisionLayers;
    particle.collide = emitter.collide;
    particle.alive = true;
    ++aliveCount_;
}

void ParticleSimulation::emit_burst(std::size_t index, std::size_t count) {
    if (index >= emitters_.size() || !emitters_[index].alive) return;
    for (std::size_t i = 0; i < count && !freeParticles_.empty(); ++i) spawn(emitters_[index]);
}

void ParticleSimulation::update(float deltaTime, const Physics::PhysicsRuntime* collisionWorld) {
    if (deltaTime <= 0.0f) return;
    const float dt = std::min(deltaTime, 0.1f);
    for (auto& emitterSlot : emitters_) if (emitterSlot.alive && emitterSlot.description.emitting && emitterSlot.description.rate > 0.0f) {
        emitterSlot.accumulator += dt * emitterSlot.description.rate;
        const std::size_t count = static_cast<std::size_t>(emitterSlot.accumulator);
        emitterSlot.accumulator -= static_cast<float>(count);
        for (std::size_t i = 0; i < count && !freeParticles_.empty(); ++i) spawn(emitterSlot);
    }

    for (std::size_t index = 0; index < particles_.size(); ++index) {
        Particle& particle = particles_[index];
        if (!particle.alive) continue;
        particle.age += dt;
        if (particle.age >= particle.lifetime) {
            particle.alive = false;
            freeParticles_.push_back(index);
            --aliveCount_;
            continue;
        }
        particle.previousPosition = particle.position;
        particle.velocity += (particle.acceleration + turbulence_vector(particle.position, particle.age) * particle.turbulence) * dt;
        particle.velocity *= 1.0f / (1.0f + std::max(0.0f, particle.drag) * dt);
        glm::vec3 nextPosition = particle.position + particle.velocity * dt;
        if (collisionWorld && particle.collide) {
            const glm::vec3 travel = nextPosition - particle.position;
            const float distance = std::sqrt(glm::dot(travel, travel));
            if (distance > 1.0e-6f) if (const auto hit = collisionWorld->raycast(particle.position, travel, distance, particle.collisionLayers)) {
                nextPosition = hit->point + hit->normal * 0.002f;
                particle.velocity = particle.velocity - (1.0f + glm::clamp(particle.restitution, 0.0f, 1.0f)) * glm::dot(particle.velocity, hit->normal) * hit->normal;
            }
        }
        particle.position = nextPosition;
        particle.rotation += particle.angularVelocity * dt;
        const float normalizedAge = particle.age / particle.lifetime;
        particle.color = glm::mix(particle.startColor, particle.endColor, normalizedAge);
        particle.size = glm::mix(particle.startSize, particle.endSize, normalizedAge);
    }
}

void ParticleSimulation::clear() {
    freeParticles_.clear();
    aliveCount_ = 0;
    for (std::size_t i = particles_.size(); i > 0; --i) {
        particles_[i - 1].alive = false;
        freeParticles_.push_back(i - 1);
    }
    for (auto& emitter : emitters_) emitter.accumulator = 0.0f;
}

std::vector<ParticleRenderData> ParticleSimulation::render_data() const {
    std::vector<ParticleRenderData> result;
    result.reserve(aliveCount_);
    for (const Particle& particle : particles_) if (particle.alive) result.push_back({particle.position, particle.color, particle.size, particle.rotation});
    return result;
}

} // namespace Engine::Gameplay
