#include "Navmesh.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>

namespace Engine::Navigation {

namespace {
constexpr float kPi = 3.14159265358979323846f;
inline float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
} // namespace

// ─── Navmesh ───
bool Navmesh::build(const NavmeshSettings& settings,
                    const std::vector<NavObstacle>& obstacles,
                    const HeightSampler& height) {
    settings_ = settings;
    valid_ = false;
    polygons_.clear();

    const glm::vec2 extent = settings.boundsMax - settings.boundsMin;
    if (extent.x <= 0 || extent.y <= 0 || settings.cellSize <= 0) return false;
    gridWidth_ = static_cast<int>(std::ceil(extent.x / settings.cellSize));
    gridHeight_ = static_cast<int>(std::ceil(extent.y / settings.cellSize));
    cells_.assign(static_cast<size_t>(gridWidth_) * gridHeight_, Cell{});
    if (!height) return false;

    // 1. Rasterize heightfield and mark walkable cells (slope test + obstacles).
    const float slopeRad = settings.maxSlope * kPi / 180.0f;
    const float slopeTan = std::tan(slopeRad);
    for (int z = 0; z < gridHeight_; ++z) {
        for (int x = 0; x < gridWidth_; ++x) {
            const float wx = settings.boundsMin.x + (x + 0.5f) * settings.cellSize;
            const float wz = settings.boundsMin.y + (z + 0.5f) * settings.cellSize;
            const float h = height(wx, wz);
            // Obstacle test (footprint with agent-radius erosion).
            bool insideObstacle = false;
            for (const NavObstacle& o : obstacles) {
                if (o.radius > 0.0f) {
                    const float dx = wx - o.center.x;
                    const float dz = wz - o.center.y;
                    if (std::sqrt(dx * dx + dz * dz) <= o.radius + settings.agentRadius) { insideObstacle = true; break; }
                } else {
                    const float dx = std::abs(wx - o.center.x);
                    const float dz = std::abs(wz - o.center.y);
                    if (dx <= o.halfExtents.x + settings.agentRadius &&
                        dz <= o.halfExtents.y + settings.agentRadius) { insideObstacle = true; break; }
                }
            }
            if (insideObstacle) continue;
            // Slope test: compare against neighbors.
            bool steep = false;
            if (x + 1 < gridWidth_ && z + 1 < gridHeight_) {
                const float hx = height(wx + settings.cellSize, wz);
                const float hz = height(wx, wz + settings.cellSize);
                const float slope = std::max(std::abs(hx - h), std::abs(hz - h)) / settings.cellSize;
                if (slope > slopeTan) steep = true;
            }
            if (steep) continue;
            cells_[index(x, z)] = Cell{true, h};
        }
    }

    // 2. Erode walkable region by agent radius (keep a border of unwalkable).
    std::vector<Cell> eroded(cells_);
    const int erosion = std::max(1, static_cast<int>(std::ceil(settings.agentRadius / settings.cellSize)));
    for (int z = 0; z < gridHeight_; ++z) {
        for (int x = 0; x < gridWidth_; ++x) {
            if (!cells_[index(x, z)].walkable) continue;
            bool nearEdge = false;
            for (int dz = -erosion; dz <= erosion && !nearEdge; ++dz) {
                for (int dx = -erosion; dx <= erosion && !nearEdge; ++dx) {
                    const int nx = x + dx, nz = z + dz;
                    if (!in_bounds(nx, nz)) { nearEdge = true; break; }
                    if (!cells_[index(nx, nz)].walkable) { nearEdge = true; break; }
                }
            }
            if (nearEdge) eroded[index(x, z)].walkable = false;
        }
    }
    cells_ = std::move(eroded);

    // 3. Group walkable cells into convex polygons (row-run merging).
    build_polygons();

    valid_ = !polygons_.empty();
    return valid_;
}

void Navmesh::build_polygons() {
    polygons_.clear();
    std::vector<uint8_t> visited(cells_.size(), 0);
    // Label connected regions (4-connectivity).
    std::vector<int> region(cells_.size(), -1);
    int regionCount = 0;
    for (int z = 0; z < gridHeight_; ++z) {
        for (int x = 0; x < gridWidth_; ++x) {
            if (!cells_[index(x, z)].walkable || region[index(x, z)] >= 0) continue;
            // BFS flood fill.
            std::queue<std::pair<int, int>> q;
            q.push({x, z});
            region[index(x, z)] = regionCount;
            while (!q.empty()) {
                auto [cx, cz] = q.front();
                q.pop();
                const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (auto& d : dirs) {
                    const int nx = cx + d[0], nz = cz + d[1];
                    if (!in_bounds(nx, nz)) continue;
                    if (cells_[index(nx, nz)].walkable && region[index(nx, nz)] < 0) {
                        region[index(nx, nz)] = regionCount;
                        q.push({nx, nz});
                    }
                }
            }
            ++regionCount;
        }
    }

    // For each region, build merged rows into convex-ish rectangles.
    for (int r = 0; r < regionCount; ++r) {
        std::vector<int> rowStart(gridHeight_, -1);
        std::vector<int> rowEnd(gridHeight_, -1);
        for (int z = 0; z < gridHeight_; ++z) {
            for (int x = 0; x < gridWidth_; ++x) {
                if (region[index(x, z)] != r) continue;
                if (rowStart[z] < 0) rowStart[z] = x;
                rowEnd[z] = x;
            }
        }
        // Greedy vertical merge of maximal rectangles.
        std::vector<uint8_t> consumedRow(gridHeight_, 0);
        for (int z0 = 0; z0 < gridHeight_; ++z0) {
            if (rowStart[z0] < 0 || consumedRow[z0]) continue;
            const int sx = rowStart[z0], ex = rowEnd[z0];
            int z1 = z0;
            while (z1 + 1 < gridHeight_ && !consumedRow[z1 + 1] &&
                   rowStart[z1 + 1] == sx && rowEnd[z1 + 1] == ex) {
                ++z1;
            }
            for (int z = z0; z <= z1; ++z) consumedRow[z] = 1;
            // Polygon: rectangle in world coords.
            NavPolygon poly;
            const float cell = settings_.cellSize;
            const float x0w = settings_.boundsMin.x + sx * cell;
            const float x1w = settings_.boundsMin.x + (ex + 1) * cell;
            const float z0w = settings_.boundsMin.y + z0 * cell;
            const float z1w = settings_.boundsMin.y + (z1 + 1) * cell;
            poly.vertices = {
                {x0w, z0w}, {x1w, z0w}, {x1w, z1w}, {x0w, z1w}
            };
            const int midZ = (z0 + z1) / 2;
            const int midX = (sx + ex) / 2;
            poly.height = cells_[index(midX, midZ)].height;
            poly.areaCost = 1.0f;
            poly.areaId = 0;
            polygons_.push_back(std::move(poly));
        }
    }
}

bool Navmesh::is_walkable(float x, float z, float y) const {
    const float nx = (x - settings_.boundsMin.x) / settings_.cellSize;
    const float nz = (z - settings_.boundsMin.y) / settings_.cellSize;
    const int ix = static_cast<int>(std::floor(nx));
    const int iz = static_cast<int>(std::floor(nz));
    if (!in_bounds(ix, iz)) return false;
    const Cell& c = cells_[index(ix, iz)];
    if (!c.walkable) return false;
    // Height tolerance: agent must be within maxClimb of the floor.
    return std::abs(y - c.height) <= settings_.maxClimb;
}

std::vector<glm::vec3> Navmesh::find_path(const glm::vec3& start, const glm::vec3& goal) const {
    // A* over the cell grid using center points, then smooth.
    const auto startCell = [&](const glm::vec3& p) -> std::optional<std::pair<int, int>> {
        const float nx = (p.x - settings_.boundsMin.x) / settings_.cellSize;
        const float nz = (p.z - settings_.boundsMin.y) / settings_.cellSize;
        const int ix = static_cast<int>(std::floor(nx));
        const int iz = static_cast<int>(std::floor(nz));
        if (!in_bounds(ix, iz) || !cells_[index(ix, iz)].walkable) return std::nullopt;
        return std::pair<int, int>{ix, iz};
    };

    const auto s = startCell(start);
    const auto g = startCell(goal);
    if (!s || !g) return {};
    if (*s == *g) return {start, goal};

    struct Node { int x, z; float g, f; int px, pz; bool closed; bool visited; };
    std::vector<Node> nodes(cells_.size());
    for (size_t i = 0; i < nodes.size(); ++i) nodes[i] = Node{0, 0, 0, 0, -1, -1, false, false};

    const auto cellCenter = [&](int x, int z) -> glm::vec3 {
        return glm::vec3(settings_.boundsMin.x + (x + 0.5f) * settings_.cellSize,
                         cells_[index(x, z)].height,
                         settings_.boundsMin.y + (z + 0.5f) * settings_.cellSize);
    };
    const auto heuristic = [&](int x, int z) -> float {
        const glm::vec3 c = cellCenter(x, z);
        return std::abs(c.x - goal.x) + std::abs(c.z - goal.z);
    };

    using PQ = std::priority_queue<std::pair<float, int>, std::vector<std::pair<float, int>>, std::greater<>>;
    PQ open;
    const int startIdx = index(s->first, s->second);
    nodes[startIdx] = Node{s->first, s->second, 0, heuristic(s->first, s->second), -1, -1, false};
    open.push({nodes[startIdx].f, startIdx});

    const int goalIdx = index(g->first, g->second);
    const int dirs[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
    const float diagCost = 1.41421356f;

    bool found = false;
    while (!open.empty()) {
        auto [f, idx] = open.top();
        open.pop();
        if (nodes[idx].closed) continue;
        nodes[idx].closed = true;
        if (idx == goalIdx) { found = true; break; }
        const int cx = nodes[idx].x, cz = nodes[idx].z;
        for (auto& d : dirs) {
            const int nx = cx + d[0], nz = cz + d[1];
            if (!in_bounds(nx, nz)) continue;
            if (!cells_[index(nx, nz)].walkable) continue;
            const int nIdx = index(nx, nz);
            if (nodes[nIdx].closed) continue;
            const bool diag = d[0] != 0 && d[1] != 0;
            if (diag) {
                // Prevent cutting corners.
                if (!cells_[index(cx + d[0], cz)].walkable || !cells_[index(cx, cz + d[1])].walkable) continue;
            }
            const float stepCost = (diag ? diagCost : 1.0f) * settings_.cellSize;
            const float heightDelta = std::abs(cells_[index(nx, nz)].height - cells_[index(cx, cz)].height);
            const float climbCost = heightDelta / std::max(settings_.maxClimb, 1e-4f);
            const float newG = nodes[idx].g + stepCost * (1.0f + climbCost * 0.5f);
            if (!nodes[nIdx].visited || newG < nodes[nIdx].g) {
                nodes[nIdx].visited = true;
                nodes[nIdx].x = nx;
                nodes[nIdx].z = nz;
                nodes[nIdx].g = newG;
                nodes[nIdx].f = newG + heuristic(nx, nz);
                nodes[nIdx].px = cx;
                nodes[nIdx].pz = cz;
                open.push({nodes[nIdx].f, nIdx});
            }
        }
    }
    if (!found) return {};

    // Reconstruct.
    std::vector<glm::vec3> raw;
    int cur = goalIdx;
    while (cur != -1) {
        raw.push_back(cellCenter(nodes[cur].x, nodes[cur].z));
        cur = (nodes[cur].px >= 0) ? index(nodes[cur].px, nodes[cur].pz) : -1;
    }
    std::reverse(raw.begin(), raw.end());
    raw.front() = start;
    raw.back() = goal;
    return smooth_path(raw, settings_.agentRadius);
}

std::vector<glm::vec3> Navmesh::smooth_path(const std::vector<glm::vec3>& path, float radius) {
    if (path.size() <= 2) return path;
    // Simple string-pulling: keep only waypoints that change direction.
    std::vector<glm::vec3> result;
    result.push_back(path.front());
    for (size_t i = 1; i + 1 < path.size(); ++i) {
        const glm::vec3 a = result.back();
        const glm::vec3 b = path[i];
        const glm::vec3 c = path[i + 1];
        const glm::vec3 ab = glm::normalize(b - a);
        const glm::vec3 bc = glm::normalize(c - b);
        // If the turn is significant, keep the waypoint.
        if (glm::dot(ab, bc) < 0.985f) result.push_back(b);
    }
    result.push_back(path.back());
    return result;
}

// ─── CrowdSimulation ───
void CrowdSimulation::step(float deltaTime) {
    for (CrowdAgent& agent : agents_) {
        if (!agent.active) continue;
        // RVO-style avoidance: sum repulsion from nearby agents, stronger when
        // they are on a collision course (approaching each other).
        glm::vec3 avoidance(0);
        for (const CrowdAgent& other : agents_) {
            if (&other == &agent || !other.active) continue;
            const glm::vec3 delta = agent.position - other.position;
            const float dist = glm::length(delta);
            const float minDist = agent.radius + other.radius + 0.15f;
            if (dist < minDist && dist > 1e-4f) {
                const float push = (minDist - dist) / minDist;
                avoidance += glm::normalize(delta) * push * 4.0f;
            } else if (dist < minDist * 2.5f && dist > 1e-4f) {
                // Predictive component: if the other agent is approaching,
                // steer sideways preemptively.
                const glm::vec3 relVel = other.velocity - agent.velocity;
                const float approaching = -glm::dot(relVel, glm::normalize(delta));
                if (approaching > 0.0f) {
                    const glm::vec3 side = glm::normalize(glm::vec3(-delta.z, 0, delta.x));
                    const float w = (1.0f - dist / (minDist * 2.5f)) * approaching * 0.8f;
                    avoidance += side * w;
                }
            }
        }
        glm::vec3 desired = agent.desiredVelocity;
        if (glm::length(desired) > agent.maxSpeed) desired = glm::normalize(desired) * agent.maxSpeed;
        const glm::vec3 targetVel = desired + avoidance * agent.maxSpeed;
        // Smooth acceleration toward target velocity.
        const float rate = std::min(1.0f, 10.0f * deltaTime);
        agent.velocity = glm::mix(agent.velocity, targetVel, rate);
        agent.position += agent.velocity * deltaTime;
    }
}

void CrowdSimulation::seek(uint32_t agentId, const glm::vec3& target, float deltaTime) {
    if (agentId >= agents_.size() || !agents_[agentId].active) return;
    CrowdAgent& agent = agents_[agentId];
    const glm::vec3 to = target - agent.position;
    const float dist = glm::length(to);
    if (dist > 0.01f) {
        agent.desiredVelocity = glm::normalize(to) * agent.maxSpeed;
    } else {
        agent.desiredVelocity = glm::vec3(0);
    }
    step(deltaTime);
}

} // namespace Engine::Navigation
