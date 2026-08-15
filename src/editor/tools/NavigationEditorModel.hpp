#pragma once
#include "EditorToolModel.hpp"
namespace Engine::Editor {
struct NavigationAreaModel{std::string name;float cost{1};uint32_t mask{1};};struct NavigationLinkModel{glm::vec3 start{0},end{0};float cost{1};bool bidirectional{true};};
class NavigationEditorModel final:public EditorDocumentModel{public:float cellSize{0.3f},cellHeight{0.2f},agentRadius{0.4f},agentHeight{1.8f},maxSlopeDegrees{45};std::vector<NavigationAreaModel>areas;std::vector<NavigationLinkModel>links;bool debugDraw{};
 void add_link(NavigationLinkModel l){links.push_back(l);changed();}
 std::vector<ValidationIssue>validate()const override{std::vector<ValidationIssue>r;if(cellSize<=0||cellHeight<=0||agentRadius<=0||agentHeight<=0)r.push_back({ValidationSeverity::Error,"bake","Navigation bake dimensions must be positive"});if(maxSlopeDegrees<0||maxSlopeDegrees>90)r.push_back({ValidationSeverity::Error,"maxSlopeDegrees","Slope must be in [0,90]"});return r;}
};}
