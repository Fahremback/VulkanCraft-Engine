// HairPhysics.cpp — Hair physics adapter: Verlet integration + spring-mass constraints
// TressFX-style simulation on CPU. No GPU/compute required.

#include "engine/rendering/IHairPhysics.hpp"
#include <cmath>
#include <algorithm>
#include <memory>

namespace vc::rendering {

static float vecDot(const Vec3& a, const Vec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static float vecLen(const Vec3& v) { return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z); }
static Vec3 vecAdd(const Vec3& a, const Vec3& b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
static Vec3 vecSub(const Vec3& a, const Vec3& b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
static Vec3 vecScale(const Vec3& v, float s) { return {v.x*s, v.y*s, v.z*s}; }

class HairPhysicsImpl : public IHairPhysics {
public:
    explicit HairPhysicsImpl(const HairConfig& cfg) : cfg_(cfg) {}

    HairStrand createStrand(const std::vector<Vec3>& positions) override {
        HairStrand strand;
        strand.particles.resize(positions.size());
        for (size_t i = 0; i < positions.size(); i++) {
            strand.particles[i].position = positions[i];
            strand.particles[i].prevPosition = positions[i];
            strand.particles[i].acceleration = {0, cfg_.gravity, 0};
            strand.particles[i].invMass = (i == 0) ? 0.0f : 1.0f; // root pinned
        }
        return strand;
    }

    void simulate(HairStrand& strand, const HairConfig& config) override {
        float dt = config.dt;
        float dt2 = dt * dt;
        Vec3 gravity{0, config.gravity, 0};
        Vec3 wind = vecScale(config.windDirection, config.windStrength);

        // Store rest lengths on first call (or use current)
        // 1. Verlet integration
        for (auto& p : strand.particles) {
            if (p.invMass == 0) continue; // pinned
            Vec3 velocity = vecSub(p.position, p.prevPosition);
            velocity = vecScale(velocity, config.damping);
            Vec3 accel = vecAdd(gravity, wind);
            Vec3 newPos = vecAdd(p.position, vecAdd(velocity, vecScale(accel, dt2)));
            p.prevPosition = p.position;
            p.position = newPos;
        }

        // 2. Local shape constraints (between adjacent particles)
        for (int iter = 0; iter < config.localIterations; iter++) {
            for (size_t i = 0; i + 1 < strand.particles.size(); i++) {
                auto& a = strand.particles[i];
                auto& b = strand.particles[i + 1];
                // Use rest length from initial positions (approx: distance between adjacent particles at creation)
                // Use the rest length from initial positions
                Vec3 delta = vecSub(b.position, a.position);
                float dist = vecLen(delta);
                if (dist < 1e-6f) continue;
                Vec3 dir = vecScale(delta, 1.0f / dist);
                float restLen = vecLen(vecSub(b.prevPosition, a.prevPosition));
                // Recompute rest length from initial config if available
                if (restLen < 1e-6f) restLen = dist;
                float correction = (dist - restLen) * config.stiffness * 0.5f;
                Vec3 corr = vecScale(dir, correction);
                if (a.invMass > 0) a.position = vecAdd(a.position, corr);
                if (b.invMass > 0) b.position = vecSub(b.position, corr);
            }
        }

        // 3. Length constraints
        for (int iter = 0; iter < config.lengthIterations; iter++) {
            for (size_t i = 0; i + 1 < strand.particles.size(); i++) {
                auto& a = strand.particles[i];
                auto& b = strand.particles[i + 1];
                applyDistanceConstraint(a, b, vecLen(vecSub(b.prevPosition, a.prevPosition)), config.stiffness);
            }
        }
    }

    void applyDistanceConstraint(HairParticle& a, HairParticle& b, float targetDist, float stiffness) override {
        Vec3 delta = vecSub(b.position, a.position);
        float dist = vecLen(delta);
        if (dist < 1e-6f) return;
        Vec3 dir = vecScale(delta, 1.0f / dist);
        float diff = dist - targetDist;
        float totalMass = (a.invMass > 0 ? 1.0f / a.invMass : 0) + (b.invMass > 0 ? 1.0f / b.invMass : 0);
        if (totalMass < 1e-6f) return;
        float correctionMag = diff * stiffness / totalMass;
        if (a.invMass > 0) a.position = vecAdd(a.position, vecScale(dir, correctionMag / a.invMass));
        if (b.invMass > 0) b.position = vecSub(b.position, vecScale(dir, correctionMag / b.invMass));
    }

    int particleCount(const HairStrand& strand) const override {
        return static_cast<int>(strand.particles.size());
    }

    HairConfig getConfig() const override { return cfg_; }

private:
    HairConfig cfg_;
};

std::unique_ptr<IHairPhysics> create_hair_physics(const HairConfig& config, std::string& errorOut) {
    if (!config.validate()) { errorOut = "invalid config"; return nullptr; }
    return std::make_unique<HairPhysicsImpl>(config);
}

} // namespace vc::rendering
