#pragma once
#include <glm/glm.hpp>
#include <vector>
#include <optional>
#include <unordered_map>

namespace Engine {
struct GridCell { int x{}; int y{}; auto operator<=>(const GridCell&) const = default; };
struct GridCellHash { size_t operator()(const GridCell& c) const noexcept { return (static_cast<size_t>(static_cast<uint32_t>(c.x))<<32)^static_cast<uint32_t>(c.y); } };
struct NavigationPath { bool success{}; std::vector<glm::vec3> points; float cost{}; };

class NavigationGrid final {
public:
    NavigationGrid()=default;
    NavigationGrid(int width,int height,float cellSize);
    int width()const noexcept{return width_;} int height()const noexcept{return height_;} float cell_size()const noexcept{return cellSize_;}
    bool valid(GridCell cell)const noexcept;
    void set_blocked(GridCell cell,bool blocked);
    bool blocked(GridCell cell)const;
    void set_cost(GridCell cell,float cost);
    float cost(GridCell cell)const;
    glm::vec3 world_position(GridCell cell)const;
    std::optional<GridCell> cell_at(const glm::vec3& world)const;
    NavigationPath find_path(GridCell start,GridCell goal)const;
private:
    int index(GridCell c)const{return c.y*width_+c.x;}
    int width_{};int height_{};float cellSize_{1};std::vector<uint8_t> blocked_;std::vector<float> costs_;
};
class NavigationAgent final {
public:
    glm::vec3 position{0};float speed{3};float stoppingDistance{0.05f};
    void set_path(NavigationPath path);void update(float deltaTime);bool reached_destination()const noexcept{return reached_;}
private:NavigationPath path_;size_t waypoint_{};bool reached_{true};
};
struct NavigationLink{glm::vec3 from{0},to{0};float cost{1};bool bidirectional{true};};
struct NavigationTile{GridCell coordinate;NavigationGrid grid;};
class NavigationWorld final{
public:
 void load_tile(GridCell coordinate,NavigationGrid grid);bool unload_tile(GridCell coordinate);bool is_tile_loaded(GridCell coordinate)const;
 void add_link(NavigationLink link);NavigationPath find_path(const glm::vec3& start,const glm::vec3& goal)const;
private:std::unordered_map<GridCell,NavigationGrid,GridCellHash> tiles_;std::vector<NavigationLink> links_;
};
}
