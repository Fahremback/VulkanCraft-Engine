#pragma once
// IFluidSimulation.hpp — Headless fluid simulation: shallow water equations
// Heightfield-based fluid for water levels, flow, waterfalls.
// No GPU, no rendering required.

#include <memory>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

namespace vc::rendering {

struct FluidConfig {
    int gridSize = 64;          // NxN heightfield
    float cellSize = 1.0f;      // world units per cell
    float gravity = 9.81f;      // gravitational acceleration
    float dt = 1.0f / 60.0f;   // time step
    int solverIterations = 4;   // Jacobi iterations per step
    float damping = 0.999f;     // velocity damping [0,1]
    float viscosity = 0.001f;   // viscosity coefficient
    float surfaceTension = 0.0f;// surface tension coefficient

    bool validate() const {
        return gridSize > 0 && cellSize > 0 && gravity > 0 && dt > 0
            && solverIterations > 0 && damping >= 0 && damping <= 1;
    }
    std::string toJson() const {
        return "{\"gridSize\":" + std::to_string(gridSize)
            + ",\"cellSize\":" + std::to_string(cellSize)
            + ",\"gravity\":" + std::to_string(gravity)
            + ",\"dt\":" + std::to_string(dt)
            + ",\"solverIterations\":" + std::to_string(solverIterations)
            + ",\"damping\":" + std::to_string(damping)
            + ",\"viscosity\":" + std::to_string(viscosity)
            + ",\"surfaceTension\":" + std::to_string(surfaceTension) + "}";
    }
    static FluidConfig fromJson(const std::string& s) {
        FluidConfig c;
        auto fi = [&](const char* k, int& v) {
            auto p = s.find(std::string("\"") + k + "\":");
            if (p != std::string::npos) v = std::stoi(s.substr(p + std::strlen(k) + 3));
        };
        auto ff = [&](const char* k, float& v) {
            auto p = s.find(std::string("\"") + k + "\":");
            if (p != std::string::npos) v = std::stof(s.substr(p + std::strlen(k) + 3));
        };
        fi("gridSize", c.gridSize); ff("cellSize", c.cellSize);
        ff("gravity", c.gravity); ff("dt", c.dt);
        fi("solverIterations", c.solverIterations);
        ff("damping", c.damping); ff("viscosity", c.viscosity);
        ff("surfaceTension", c.surfaceTension);
        return c;
    }
};

struct FluidState {
    std::vector<float> height;    // water height per cell
    std::vector<float> velocity;  // horizontal velocity (x-component, packed)
    std::vector<float> velocityY; // vertical velocity (z-component, packed)
};

class IFluidSimulation {
public:
    virtual ~IFluidSimulation() = default;

    // Create a grid state initialized to zero
    virtual FluidState createState() const = 0;

    // Set height of a cell (for initialization)
    virtual void setHeight(FluidState& state, int x, int z, float h) const = 0;

    // Get height of a cell
    virtual float getHeight(const FluidState& state, int x, int z) const = 0;

    // Step simulation (shallow water equations: continuity + momentum)
    virtual void simulate(FluidState& state, const FluidConfig& config) const = 0;

    // Add a source (e.g., rain, fountain) at a cell
    virtual void addSource(FluidState& state, int x, int z, float amount) const = 0;

    // Get total water volume (sum of heights * cellArea)
    virtual float totalVolume(const FluidState& state, const FluidConfig& config) const = 0;

    // Get max height in the grid
    virtual float maxHeight(const FluidState& state) const = 0;

    // Get config
    virtual FluidConfig getConfig() const = 0;
};

std::unique_ptr<IFluidSimulation> create_fluid_simulation(const FluidConfig& config, std::string& errorOut);

} // namespace vc::rendering
