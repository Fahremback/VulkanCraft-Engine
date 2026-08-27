#pragma once
// IHairPhysics.hpp — Headless hair physics: Verlet integration + spring-mass constraints
// Wraps TressFX simulation algorithms as a self-contained contract.
// No GPU, no compute shaders, no rendering required.

#include <memory>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

namespace vc::rendering {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

struct HairParticle {
    Vec3 position;
    Vec3 prevPosition;  // for Verlet
    Vec3 acceleration;
    float invMass = 1.0f; // 0 = pinned
};

struct HairStrand {
    std::vector<HairParticle> particles;
};

struct HairConfig {
    float gravity = -9.81f;        // Y axis
    float damping = 0.99f;         // velocity damping [0,1]
    float stiffness = 0.9f;        // local constraint stiffness [0,1]
    int localIterations = 3;       // local shape constraint iterations
    int lengthIterations = 3;      // length constraint iterations
    float globalStiffness = 0.0f;  // global shape constraint stiffness [0,1]
    float globalRange = 0.0f;      // global shape constraint range
    float windStrength = 0.0f;
    Vec3 windDirection{0,0,0};
    float dt = 1.0f / 60.0f;

    bool validate() const {
        return damping >= 0 && damping <= 1 && stiffness >= 0 && stiffness <= 1
            && localIterations >= 0 && lengthIterations >= 0 && dt > 0;
    }
    std::string toJson() const {
        return "{\"gravity\":" + std::to_string(gravity)
            + ",\"damping\":" + std::to_string(damping)
            + ",\"stiffness\":" + std::to_string(stiffness)
            + ",\"localIterations\":" + std::to_string(localIterations)
            + ",\"lengthIterations\":" + std::to_string(lengthIterations)
            + ",\"globalStiffness\":" + std::to_string(globalStiffness)
            + ",\"globalRange\":" + std::to_string(globalRange)
            + ",\"windStrength\":" + std::to_string(windStrength)
            + ",\"dt\":" + std::to_string(dt) + "}";
    }
    static HairConfig fromJson(const std::string& s) {
        HairConfig c;
        auto f = [&](const char* key, float& v) {
            std::string ks(key);
            auto p = s.find(std::string("\"") + ks + "\":");
            if (p != std::string::npos) v = std::stof(s.substr(p + ks.size() + 3));
        };
        auto i = [&](const char* key, int& v) {
            std::string ks(key);
            auto p = s.find(std::string("\"") + ks + "\":");
            if (p != std::string::npos) v = std::stoi(s.substr(p + ks.size() + 3));
        };
        f("gravity", c.gravity); f("damping", c.damping); f("stiffness", c.stiffness);
        i("localIterations", c.localIterations); i("lengthIterations", c.lengthIterations);
        f("globalStiffness", c.globalStiffness); f("globalRange", c.globalRange);
        f("windStrength", c.windStrength); f("dt", c.dt);
        return c;
    }
};

class IHairPhysics {
public:
    virtual ~IHairPhysics() = default;

    // Create a strand from positions (root to tip)
    virtual HairStrand createStrand(const std::vector<Vec3>& positions) = 0;

    // Step simulation: Verlet integration + constraints + wind
    virtual void simulate(HairStrand& strand, const HairConfig& config) = 0;

    // Apply distance constraint between two particles (spring-mass)
    virtual void applyDistanceConstraint(HairParticle& a, HairParticle& b, float targetDist, float stiffness) = 0;

    // Get particle count
    virtual int particleCount(const HairStrand& strand) const = 0;

    // Get config
    virtual HairConfig getConfig() const = 0;
};

std::unique_ptr<IHairPhysics> create_hair_physics(const HairConfig& config, std::string& errorOut);

} // namespace vc::rendering
