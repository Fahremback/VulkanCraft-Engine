#pragma once

#include "../physics/PhysicsRuntime.hpp"

#include <cstdint>
#include <vector>

namespace Engine::Gameplay {

struct Particle {
    glm::vec3 position{0.0f};
    glm::vec3 previousPosition{0.0f};
    glm::vec3 velocity{0.0f};
    glm::vec4 color{1.0f};
    float size{0.1f};
    float rotation{0.0f};
    float angularVelocity{0.0f};
    float age{0.0f};
    float lifetime{1.0f};
    glm::vec4 startColor{1.0f};
    glm::vec4 endColor{1.0f, 1.0f, 1.0f, 0.0f};
    glm::vec3 acceleration{0.0f};
    float startSize{0.1f};
    float endSize{0.0f};
    float drag{0.0f};
    float turbulence{0.0f};
    float restitution{0.0f};
    std::uint32_t collisionLayers{~0u};
    bool collide{false};
    bool alive{false};
};

struct ParticleEmitterDesc {
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, 1.0f, 0.0f};
    float coneAngle{0.4f};
    float rate{20.0f};
    float speedMin{1.0f};
    float speedMax{3.0f};
    float lifetimeMin{0.5f};
    float lifetimeMax{1.5f};
    float sizeStart{0.12f};
    float sizeEnd{0.0f};
    glm::vec4 colorStart{1.0f};
    glm::vec4 colorEnd{1.0f, 1.0f, 1.0f, 0.0f};
    glm::vec3 acceleration{0.0f, -9.81f, 0.0f};
    float drag{0.05f};
    float turbulence{0.0f};
    float restitution{0.35f};
    std::uint32_t collisionLayers{~0u};
    bool collide{false};
    bool emitting{true};
};

struct ParticleRenderData {
    glm::vec3 position{0.0f};
    glm::vec4 color{1.0f};
    float size{0.0f};
    float rotation{0.0f};
};

class ParticleSimulation final {
public:
    explicit ParticleSimulation(std::size_t capacity = 4096, std::uint32_t seed = 0x853c49e6u);

    std::size_t add_emitter(const ParticleEmitterDesc& description);
    bool remove_emitter(std::size_t emitter);
    ParticleEmitterDesc* emitter(std::size_t emitter);
    const ParticleEmitterDesc* emitter(std::size_t emitter) const;
    void emit_burst(std::size_t emitter, std::size_t count);
    void update(float deltaTime, const Physics::PhysicsRuntime* collisionWorld = nullptr);
    void clear();

    std::size_t alive_count() const noexcept { return aliveCount_; }
    std::size_t capacity() const noexcept { return particles_.size(); }
    const std::vector<Particle>& particles() const noexcept { return particles_; }
    std::vector<ParticleRenderData> render_data() const;

private:
    struct EmitterSlot { ParticleEmitterDesc description; float accumulator{0.0f}; bool alive{true}; };
    float random_unit();
    glm::vec3 random_direction(const glm::vec3& direction, float coneAngle);
    void spawn(EmitterSlot& emitter);

    std::vector<Particle> particles_;
    std::vector<std::size_t> freeParticles_;
    std::vector<EmitterSlot> emitters_;
    std::size_t aliveCount_{0};
    std::uint32_t randomState_{0};
};

} // namespace Engine::Gameplay
