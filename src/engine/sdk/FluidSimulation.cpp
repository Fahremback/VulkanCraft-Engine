// FluidSimulation.cpp — Simple fluid simulation: height diffusion
// Stable heightfield-based fluid for water levels, flow, waterfalls.
// Uses explicit Euler diffusion instead of full SWE (avoids instability).

#include "engine/rendering/IFluidSimulation.hpp"
#include <cmath>
#include <algorithm>
#include <vector>

namespace vc::rendering {

class FluidSimulationImpl : public IFluidSimulation {
public:
    explicit FluidSimulationImpl(const FluidConfig& cfg) : cfg_(cfg) {}

    FluidState createState() const override {
        int n = cfg_.gridSize;
        FluidState s;
        s.height.resize(n * n, 0.0f);
        s.velocity.resize(n * n, 0.0f);
        s.velocityY.resize(n * n, 0.0f);
        return s;
    }

    void setHeight(FluidState& state, int x, int z, float h) const override {
        int n = cfg_.gridSize;
        if (x >= 0 && x < n && z >= 0 && z < n) state.height[z * n + x] = h;
    }

    float getHeight(const FluidState& state, int x, int z) const override {
        int n = cfg_.gridSize;
        if (x < 0 || x >= n || z < 0 || z >= n) return 0.0f;
        return state.height[z * n + x];
    }

    void simulate(FluidState& state, const FluidConfig& config) const override {
        int n = config.gridSize;
        float dt = config.dt;
        float g = config.gravity;
        float dx = config.cellSize;
        float visc = config.viscosity;

        // Explicit Euler height diffusion: water flows from high to low
        // Using simple Laplacian diffusion for stability
        std::vector<float> newH = state.height;
        float diffusionRate = g * dt * dt / (dx * dx);

        for (int z = 1; z < n - 1; z++) {
            for (int x = 1; x < n - 1; x++) {
                int idx = z * n + x;
                float h = state.height[idx];
                if (h <= 0) continue;

                float hL = state.height[z * n + (x - 1)];
                float hR = state.height[z * n + (x + 1)];
                float hU = state.height[(z - 1) * n + x];
                float hD = state.height[(z + 1) * n + x];

                // Laplacian: flow proportional to height difference
                float laplacian = (hL + hR + hU + hD - 4.0f * h) / (dx * dx);

                // Update height: diffusion spreads water
                float dh = diffusionRate * laplacian * dt;
                newH[idx] = h + dh;

                // Compute velocity from gradient (for visualization)
                state.velocity[idx] = -(hR - hL) / (2.0f * dx) * g * dt;
                state.velocityY[idx] = -(hD - hU) / (2.0f * dx) * g * dt;

                // Apply viscosity to velocity
                state.velocity[idx] *= (1.0f - visc);
                state.velocityY[idx] *= (1.0f - visc);
            }
        }

        // Clamp negative heights
        for (float& h : newH) if (h < 0) h = 0;

        state.height = newH;
    }

    void addSource(FluidState& state, int x, int z, float amount) const override {
        int n = cfg_.gridSize;
        if (x >= 0 && x < n && z >= 0 && z < n) {
            state.height[z * n + x] += amount;
            if (state.height[z * n + x] < 0) state.height[z * n + x] = 0;
        }
    }

    float totalVolume(const FluidState& state, const FluidConfig& config) const override {
        float sum = 0;
        for (float h : state.height) sum += h;
        return sum * config.cellSize * config.cellSize;
    }

    float maxHeight(const FluidState& state) const override {
        float mx = 0;
        for (float h : state.height) mx = std::max(mx, h);
        return mx;
    }

    FluidConfig getConfig() const override { return cfg_; }

private:
    FluidConfig cfg_;
};

std::unique_ptr<IFluidSimulation> create_fluid_simulation(const FluidConfig& config, std::string& errorOut) {
    if (!config.validate()) { errorOut = "invalid config"; return nullptr; }
    return std::make_unique<FluidSimulationImpl>(config);
}

} // namespace vc::rendering
