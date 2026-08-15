#pragma once
#include "EditorToolModel.hpp"
namespace Engine::Editor {
enum class ColliderKind:uint8_t{Box,Sphere,Capsule,ConvexHull,TriangleMesh};struct ColliderModel{ColliderKind kind{};glm::vec3 center{0},size{1};float radius{0.5f},height{1};bool trigger{};UUID material{0,0};};struct ConstraintModel{std::string type;UUID connectedEntity{0,0};glm::vec3 linearLimit{0},angularLimitDegrees{0};bool collisionEnabled{};};
class PhysicsEditorModel final:public EditorDocumentModel{public:float mass{1};glm::vec3 centerOfMass{0};bool kinematic{},continuousCollision{};std::vector<ColliderModel>colliders;std::vector<ConstraintModel>constraints;
 void add_collider(ColliderModel c){colliders.push_back(c);changed();}void add_constraint(ConstraintModel c){constraints.push_back(std::move(c));changed();}
 std::vector<ValidationIssue>validate()const override{std::vector<ValidationIssue>r;if(!kinematic&&mass<=0)r.push_back({ValidationSeverity::Error,"mass","Dynamic body mass must be positive"});for(auto&c:colliders)if(c.radius<=0||c.height<=0||glm::any(glm::lessThanEqual(c.size,glm::vec3(0))))r.push_back({ValidationSeverity::Error,"collider","Collider dimensions must be positive"});return r;}
};}
