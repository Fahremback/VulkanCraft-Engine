#pragma once

// IParticleSystem — Agente 1 (task_plan C): the PUBLIC contract for CPU
// particle VFX (promoted from external/solutions/effekseer, MIT). Loads a
// compiled .efk effect from memory and simulates it deterministically
// (spawn with an explicit random seed, step by dt, query alive instances).
// The GPU renderer (EffekseerRendererVulkan) is the seam and stays
// HUMAN-VISUAL-PENDING; this surface is the simulation only.
//
// Self-contained std; no external headers. Deterministic: the same load +
// spawn(seed) + step sequence produces identical instance counts. All-or-
// nothing: invalid/unsupported effect data is refused with a diagnostic.

#include <cstdint>
#include <memory>
#include <string>

namespace Engine::Rendering {

class IParticleSystem {
public:
    virtual ~IParticleSystem() = default;

    // Parses the compiled .efk effect from `data` (the bytes are parsed into
    // internal structures; `data` need not outlive the call). Returns false
    // with a diagnostic on null/empty/unsupported data and leaves the system
    // unloaded.
    virtual bool loadEffect(const std::uint8_t* data, std::size_t size,
                            std::string& errorOut) = 0;

    // Spawns one instance at (x, y, z) with the given random seed (controls
    // the deterministic simulation). Returns the instance handle (>= 0) or -1
    // if no effect is loaded or the spawn fails.
    virtual std::int32_t spawn(float x, float y, float z,
                               std::int32_t seed) = 0;

    // Advances the simulation by `dt` seconds (all spawned instances).
    virtual void step(float dt) = 0;

    // Number of alive instances of `handle` (0 if invalid or expired).
    virtual std::int32_t aliveCount(std::int32_t handle) const = 0;

    // Stops `handle`; its instances expire on subsequent steps.
    virtual void stop(std::int32_t handle) = 0;
};

// Creates the particle system (backed by the Effekseer CPU core).
std::unique_ptr<IParticleSystem> create_particle_system();

}  // namespace Engine::Rendering
