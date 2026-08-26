// EffekseerParticles.cpp — Agente 1 (task_plan C): the ONLY TU that includes
// the vendored Effekseer headers (external/solutions/effekseer/Dev/Cpp/
// Effekseer, MIT). Maps the CPU particle-simulation core to the public
// contract engine/rendering/IParticleSystem.hpp. Pure CPU, deterministic
// (explicit per-spawn seed), all-or-nothing.
//
// BUG-010 lesson: the vendored core .cpp files are compiled INTO vc_sdk_public
// (see CMakeLists.txt) so the Effekseer symbols travel with this adapter's
// objects.

#include "engine/rendering/IParticleSystem.hpp"

#include "Effekseer.h"

#include <memory>
#include <string>

namespace Engine::Rendering {
namespace {

class EffekseerParticles final : public IParticleSystem {
public:
    EffekseerParticles() : manager_(Effekseer::Manager::Create(10000)) {}

    bool loadEffect(const std::uint8_t* data, std::size_t size,
                    std::string& errorOut) override {
        if (manager_ == nullptr) {
            errorOut = "particles: manager unavailable";
            return false;
        }
        if (data == nullptr || size == 0 || size > 0x7FFFFFFF) {
            errorOut = "particles: null or empty effect data";
            return false;
        }
        effect_ = Effekseer::Effect::Create(
            manager_, data, static_cast<std::int32_t>(size));
        if (effect_ == nullptr) {
            errorOut = "particles: invalid or unsupported .efk data";
            return false;
        }
        return true;
    }

    std::int32_t spawn(float x, float y, float z,
                       std::int32_t seed) override {
        if (manager_ == nullptr || effect_ == nullptr) {
            return -1;
        }
        const Effekseer::Handle handle = manager_->Play(effect_, x, y, z);
        if (handle < 0) {
            return -1;
        }
        manager_->SetRandomSeed(handle, seed);
        return static_cast<std::int32_t>(handle);
    }

    void step(float dt) override {
        if (manager_ == nullptr) {
            return;
        }
        // Effekseer advances in 60 fps frames; dt is in seconds.
        manager_->BeginUpdate();
        manager_->Update(dt * 60.0f);
        manager_->EndUpdate();
    }

    std::int32_t aliveCount(std::int32_t handle) const override {
        if (manager_ == nullptr || handle < 0) {
            return 0;
        }
        return manager_->GetInstanceCount(
            static_cast<Effekseer::Handle>(handle));
    }

    void stop(std::int32_t handle) override {
        if (manager_ == nullptr || handle < 0) {
            return;
        }
        manager_->StopEffect(static_cast<Effekseer::Handle>(handle));
    }

private:
    Effekseer::ManagerRef manager_;
    Effekseer::EffectRef effect_;
};

}  // namespace

std::unique_ptr<IParticleSystem> create_particle_system() {
    return std::make_unique<EffekseerParticles>();
}

}  // namespace Engine::Rendering
