// RenderingPresets.cpp — Agente 1 (task_plan A.16): the HEADLESS quality
// presets. Resolves a quality level into the budgets of every rendering
// contract; validates a preset across all sub-configs (all-or-nothing, the
// same ranges the adapters enforce). Self-contained (std + glm).

#include "engine/rendering/IRenderingPresets.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

namespace Engine::Rendering {
namespace {

constexpr std::array<const char*, 5> kLevelNames{ "Low", "Medium", "High", "Ultra",
                                                  "Cinematic" };

std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Deterministic per-level presets (progressive budget scaling). These are the
// SINGLE source of truth for quality; every consumer reads from here.
RenderingPreset make_preset(QualityLevel level) {
    RenderingPreset p;
    // --- GI clipmaps (A.2) ---
    p.gi.cascadeCount = 6;
    p.gi.resolution = 16;
    p.gi.baseSpacing = 4.0f;
    p.gi.cascadeScale = 4.0f;
    p.gi.sunRefreshAngleDegrees = 2.0f;
    // --- Reflection (A.1) ---
    p.reflection.maxScreenRays = 4096;
    p.reflection.screenRoughnessLimit = 0.45f;
    p.reflection.probeRoughnessFloor = 0.45f;
    // --- Software trace (A.7) ---
    p.trace.maxSteps = 256;
    p.trace.maxDistance = 512.0f;
    p.trace.hitEpsilon = 0.01f;
    p.trace.normalEpsilon = 0.1f;
    // --- Capture (A.4) ---
    p.capture.cardsPerFrame = 64;
    p.capture.maxCapturedCards = 4096;
    // --- Diffuse GI (A.5) ---
    p.diffuseGi.bounces = 2;
    p.diffuseGi.skylight = glm::vec3(0.05f, 0.07f, 0.10f);
    p.diffuseGi.maxDistance = 128.0f;
    p.diffuseGi.intensity = 1.0f;

    switch (level) {
        case QualityLevel::Low:
            p.gi.resolution = 8;
            p.gi.probesPerFrame = 64;
            p.reflection.maxScreenRays = 1024;
            p.trace.maxSteps = 64;
            p.trace.maxDistance = 256.0f;
            p.capture.cardsPerFrame = 16;
            p.capture.maxCapturedCards = 1024;
            p.diffuseGi.bounces = 1;
            break;
        case QualityLevel::Medium:
            p.gi.resolution = 12;
            p.gi.probesPerFrame = 128;
            p.reflection.maxScreenRays = 2048;
            p.trace.maxSteps = 128;
            p.trace.maxDistance = 384.0f;
            p.capture.cardsPerFrame = 32;
            p.capture.maxCapturedCards = 2048;
            p.diffuseGi.bounces = 1;
            break;
        case QualityLevel::High:
            p.gi.probesPerFrame = 192;
            break;  // defaults
        case QualityLevel::Ultra:
            p.gi.resolution = 24;
            p.gi.probesPerFrame = 384;
            p.reflection.maxScreenRays = 8192;
            p.trace.maxSteps = 512;
            p.trace.maxDistance = 768.0f;
            p.capture.cardsPerFrame = 128;
            p.capture.maxCapturedCards = 8192;
            p.diffuseGi.bounces = 3;
            break;
        case QualityLevel::Cinematic:
            p.gi.resolution = 32;
            p.gi.probesPerFrame = 768;
            p.reflection.maxScreenRays = 16384;
            p.trace.maxSteps = 1024;
            p.trace.maxDistance = 1024.0f;
            p.trace.hitEpsilon = 0.005f;
            p.capture.cardsPerFrame = 256;
            p.capture.maxCapturedCards = 16384;
            p.diffuseGi.bounces = 4;
            p.diffuseGi.intensity = 1.25f;
            break;
        default:
            break;
    }
    return p;
}

class RenderingPresets final : public IRenderingPresets {
public:
    RenderingPresets() {
        presets_[0] = make_preset(QualityLevel::Low);
        presets_[1] = make_preset(QualityLevel::Medium);
        presets_[2] = make_preset(QualityLevel::High);
        presets_[3] = make_preset(QualityLevel::Ultra);
        presets_[4] = make_preset(QualityLevel::Cinematic);
    }

    const RenderingPreset& preset(QualityLevel level) const noexcept override {
        const std::size_t i = static_cast<std::size_t>(level);
        return i < presets_.size() ? presets_[i] : presets_[2];
    }

    bool preset_by_name(const std::string& name,
                        RenderingPreset& out) const override {
        const std::string key = lowercase(name);
        for (std::size_t i = 0; i < kLevelNames.size(); ++i) {
            if (lowercase(kLevelNames[i]) == key) {
                out = presets_[i];
                return true;
            }
        }
        return false;
    }

    const char* name(QualityLevel level) const noexcept override {
        const std::size_t i = static_cast<std::size_t>(level);
        return i < kLevelNames.size() ? kLevelNames[i] : "High";
    }

    bool validate(const RenderingPreset& p, std::string& errorOut) const override {
        // GI (A.2 ranges).
        if (p.gi.cascadeCount < 1 || p.gi.cascadeCount > 6) {
            errorOut = "preset: gi.cascadeCount must be in [1, 6]";
            return false;
        }
        if (p.gi.resolution < 4 || p.gi.resolution > 32) {
            errorOut = "preset: gi.resolution must be in [4, 32]";
            return false;
        }
        if (p.gi.probesPerFrame < 1) {
            errorOut = "preset: gi.probesPerFrame must be >= 1";
            return false;
        }
        if (p.gi.baseSpacing < 0.5f || p.gi.cascadeScale < 2.0f) {
            errorOut = "preset: gi baseSpacing/cascadeScale out of range";
            return false;
        }
        // Reflection (A.1 ranges).
        if (p.reflection.maxScreenRays < 1) {
            errorOut = "preset: reflection.maxScreenRays must be >= 1";
            return false;
        }
        if (p.reflection.screenRoughnessLimit < 0.0f ||
            p.reflection.screenRoughnessLimit > 1.0f ||
            p.reflection.probeRoughnessFloor < 0.0f ||
            p.reflection.probeRoughnessFloor > 1.0f) {
            errorOut = "preset: reflection roughness limits must be in [0, 1]";
            return false;
        }
        // Software trace (A.7 ranges).
        if (p.trace.maxSteps < 1 || p.trace.maxDistance <= 0.0f ||
            p.trace.hitEpsilon <= 0.0f || p.trace.normalEpsilon <= 0.0f) {
            errorOut = "preset: trace bounds must be positive";
            return false;
        }
        // Capture (A.4 ranges).
        if (p.capture.cardsPerFrame < 1 || p.capture.maxCapturedCards < 1) {
            errorOut = "preset: capture budgets must be >= 1";
            return false;
        }
        // Diffuse GI (A.5 ranges).
        if (p.diffuseGi.bounces < 1 || p.diffuseGi.bounces > 8) {
            errorOut = "preset: diffuseGi.bounces must be in [1, 8]";
            return false;
        }
        if (p.diffuseGi.maxDistance <= 0.0f) {
            errorOut = "preset: diffuseGi.maxDistance must be > 0";
            return false;
        }
        if (p.diffuseGi.intensity < 0.01f || p.diffuseGi.intensity > 64.0f) {
            errorOut = "preset: diffuseGi.intensity must be in [0.01, 64]";
            return false;
        }
        errorOut.clear();
        return true;
    }

    std::vector<std::string> level_names() const override {
        return { kLevelNames.begin(), kLevelNames.end() };
    }

private:
    std::array<RenderingPreset, 5> presets_{};
};

}  // namespace

std::unique_ptr<IRenderingPresets> create_rendering_presets(std::string& errorOut) {
    auto presets = std::make_unique<RenderingPresets>();
    // Every built-in preset must validate (self-check at construction).
    for (int i = 0; i < static_cast<int>(QualityLevel::Count); ++i) {
        if (!presets->validate(presets->preset(static_cast<QualityLevel>(i)),
                               errorOut))
            return nullptr;
    }
    return presets;
}

}  // namespace Engine::Rendering
