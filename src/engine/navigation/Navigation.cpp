#include "engine/navigation/Navigation.hpp"
#include <queue>
#include <cmath>
#include <algorithm>

namespace Engine {

NavigationGrid::NavigationGrid(int width, int height, float cellSize)
    : width_(width), height_(height), cellSize_(cellSize) {
    blocked_.resize(width * height, 0);
    costs_.resize(width * height, 1.0f);
}

bool NavigationGrid::valid(GridCell cell) const noexcept {
    return cell.x >= 0 && cell.x < width_ && cell.y >= 0 && cell.y < height_;
}

void NavigationGrid::set_blocked(GridCell cell, bool blocked) {
    if (valid(cell)) blocked_[index(cell)] = blocked ? 1 : 0;
}

bool NavigationGrid::blocked(GridCell cell) const {
    return valid(cell) ? blocked_[index(cell)] != 0 : true;
}

void NavigationGrid::set_cost(GridCell cell, float cost) {
    if (valid(cell)) costs_[index(cell)] = cost;
}

float NavigationGrid::cost(GridCell cell) const {
    return valid(cell) ? costs_[index(cell)] : 10000.0f;
}

glm::vec3 NavigationGrid::world_position(GridCell cell) const {
    return glm::vec3((cell.x + 0.5f) * cellSize_, 0.0f, (cell.y + 0.5f) * cellSize_);
}

std::optional<GridCell> NavigationGrid::cell_at(const glm::vec3& world) const {
    int x = static_cast<int>(std::floor(world.x / cellSize_));
    int y = static_cast<int>(std::floor(world.z / cellSize_));
    GridCell c{x, y};
    if (valid(c)) return c;
    return std::nullopt;
}

NavigationPath NavigationGrid::find_path(GridCell start, GridCell goal) const {
    NavigationPath path;
    if (!valid(start) || !valid(goal) || blocked(goal)) return path;

    struct Node {
        GridCell cell; float g; float f;
        bool operator>(const Node& other) const { return f > other.f; }
    };
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
    std::unordered_map<GridCell, GridCell, GridCellHash> cameFrom;
    std::unordered_map<GridCell, float, GridCellHash> gScore;

    gScore[start] = 0.0f;
    open.push({start, 0.0f, 0.0f});

    const int dx[] = {1, 0, -1, 0, 1, -1, -1, 1};
    const int dy[] = {0, 1, 0, -1, 1, 1, -1, -1};

    while (!open.empty()) {
        auto current = open.top().cell;
        open.pop();

        if (current == goal) {
            path.success = true;
            GridCell curr = goal;
            while (curr != start) {
                path.points.push_back(world_position(curr));
                curr = cameFrom[curr];
            }
            path.points.push_back(world_position(start));
            std::reverse(path.points.begin(), path.points.end());
            path.cost = gScore[goal];
            return path;
        }

        for (int i = 0; i < 8; i++) {
            GridCell neighbor{current.x + dx[i], current.y + dy[i]};
            if (!valid(neighbor) || blocked(neighbor)) continue;
            
            float moveCost = (i < 4) ? 1.0f : 1.414f;
            float tentativeG = gScore[current] + moveCost * cost(neighbor);

            if (!gScore.count(neighbor) || tentativeG < gScore[neighbor]) {
                cameFrom[neighbor] = current;
                gScore[neighbor] = tentativeG;
                float h = std::abs(goal.x - neighbor.x) + std::abs(goal.y - neighbor.y);
                open.push({neighbor, tentativeG, tentativeG + h});
            }
        }
    }
    return path;
}

void NavigationAgent::set_path(NavigationPath path) {
    path_ = std::move(path);
    waypoint_ = 0;
    reached_ = path_.points.empty();
}

void NavigationAgent::update(float deltaTime) {
    if (reached_) return;
    if (waypoint_ >= path_.points.size()) {
        reached_ = true;
        return;
    }

    float remaining = speed * deltaTime;
    while (remaining > 0.0f && waypoint_ < path_.points.size()) {
        const glm::vec3 target = path_.points[waypoint_];
        glm::vec3 dir = target - position;
        const float dist = glm::length(dir);

        if (dist <= stoppingDistance) {
            position = target;
            ++waypoint_;
            continue;
        }

        const float step = std::min(remaining, dist);
        position += (dir / dist) * step;
        remaining -= step;

        if (step >= dist - 1e-4f) {
            position = target;
            ++waypoint_;
        }
    }

    if (waypoint_ >= path_.points.size()) {
        reached_ = true;
    }
}

void NavigationWorld::load_tile(GridCell coordinate, NavigationGrid grid) {
    tiles_[coordinate] = std::move(grid);
}
bool NavigationWorld::unload_tile(GridCell coordinate) {
    return tiles_.erase(coordinate) > 0;
}
bool NavigationWorld::is_tile_loaded(GridCell coordinate) const {
    return tiles_.count(coordinate) > 0;
}
void NavigationWorld::add_link(NavigationLink link) {
    links_.push_back(std::move(link));
}
NavigationPath NavigationWorld::find_path(const glm::vec3& start, const glm::vec3& goal) const {
    if (tiles_.empty()) return NavigationPath{};

    const auto startTile = tiles_.begin()->second.cell_at(start);
    const auto goalTile = tiles_.begin()->second.cell_at(goal);
    if (!startTile || !goalTile) return NavigationPath{};

    auto it = tiles_.begin();
    const NavigationGrid& grid = it->second;
    NavigationPath path = grid.find_path(*startTile, *goalTile);
    if (path.success) {
        path.points.front() = start;
        path.points.back() = goal;
    }
    return path;
}

} // namespace Engine
