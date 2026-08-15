#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <optional>
#include <memory>
#include <functional>
#include <cstdint>

namespace Engine::Navigation {

// A convex navigation polygon (2D, Y up) produced by navmesh baking.
struct NavPolygon {
    std::vector<glm::vec2> vertices;   // clockwise, Y = height baked into centroid
    float height{0.0f};                // walkable height of the polygon
    float areaCost{1.0f};              // traversal cost multiplier
    uint32_t areaId{0};                // area type (0 = walkable)
};

struct NavmeshSettings {
    glm::vec2 boundsMin{-100, -100};
    glm::vec2 boundsMax{100, 100};
    float cellSize{0.5f};      // rasterization resolution
    float agentRadius{0.3f};   // erosion radius
    float maxClimb{0.5f};      // max step height between adjacent cells
    float maxSlope{45.0f};     // degrees; slopes steeper than this are unwalkable
};

// Obstacles fed to the baker: either a box footprint or a circle.
struct NavObstacle {
    glm::vec2 center{0};
    glm::vec2 halfExtents{0.5f};
    float radius{0.0f};        // 0 = box, >0 = circle
    float height{100.0f};      // obstacles shorter than agent clearance are ignored
};

// Height sample callback: returns terrain height at (x, z) in world space.
using HeightSampler = std::function<float(float x, float z)>;

// The baked navmesh: grid of walkable cells + convex polygons + neighbor links.
class Navmesh final {
public:
    bool build(const NavmeshSettings& settings,
               const std::vector<NavObstacle>& obstacles,
               const HeightSampler& height);

    bool is_valid() const noexcept { return valid_; }
    const NavmeshSettings& settings() const noexcept { return settings_; }
    const std::vector<NavPolygon>& polygons() const noexcept { return polygons_; }

    // World-space queries.
    bool is_walkable(float x, float z, float y) const;
    std::vector<glm::vec3> find_path(const glm::vec3& start, const glm::vec3& goal) const;
    // Funnel-smoothing pass over a polyline path.
    static std::vector<glm::vec3> smooth_path(const std::vector<glm::vec3>& path, float radius);

private:
    struct Cell { bool walkable{false}; float height{0.0f}; };
    int index(int x, int z) const { return z * gridWidth_ + x; }
    bool in_bounds(int x, int z) const { return x >= 0 && z >= 0 && x < gridWidth_ && z < gridHeight_; }
    void build_polygons();

    NavmeshSettings settings_;
    int gridWidth_{0};
    int gridHeight_{0};
    std::vector<Cell> cells_;
    std::vector<NavPolygon> polygons_;
    bool valid_{false};
};

// Crowd agent with simple RVO-style neighbor avoidance.
struct CrowdAgent {
    glm::vec3 position{0};
    glm::vec3 velocity{0};
    glm::vec3 desiredVelocity{0};
    float radius{0.4f};
    float maxSpeed{3.0f};
    float mass{1.0f};
    bool active{true};
    uint32_t id{0};
};

class CrowdSimulation final {
public:
    CrowdSimulation() = default;
    void set_agents(const std::vector<CrowdAgent>& agents) { agents_ = agents; }
    const std::vector<CrowdAgent>& agents() const noexcept { return agents_; }
    std::vector<CrowdAgent>& agents() noexcept { return agents_; }
    // One simulation step: computes avoidance-adjusted velocities.
    void step(float deltaTime);
    // Simple leader-follow / seek behavior.
    void seek(uint32_t agentId, const glm::vec3& target, float deltaTime);

private:
    std::vector<CrowdAgent> agents_;
};

} // namespace Engine::Navigation
