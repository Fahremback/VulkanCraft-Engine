#pragma once
#include "EditorToolModel.hpp"
namespace Engine::Editor {
enum class RagdollShape:uint8_t{Box,Sphere,Capsule};
struct RagdollBodyModel{std::string bone;RagdollShape shape{RagdollShape::Capsule};glm::vec3 size{0.2f,0.5f,0.2f};float mass{1};float animationWeight{};};
struct RagdollJointModel{std::string parentBone,childBone;glm::vec3 angularLimitDegrees{45};};
class RagdollEditorModel final:public EditorDocumentModel{public:std::vector<RagdollBodyModel>bodies;std::vector<RagdollJointModel>joints;float globalBlend{1};
 void add_body(RagdollBodyModel b){bodies.push_back(std::move(b));changed();}void add_joint(RagdollJointModel j){joints.push_back(std::move(j));changed();}
 std::vector<ValidationIssue> validate()const override{std::vector<ValidationIssue>r;for(auto&b:bodies)if(b.bone.empty()||b.mass<=0||glm::any(glm::lessThanEqual(b.size,glm::vec3(0))))r.push_back({ValidationSeverity::Error,b.bone,"Body needs a bone, positive size and mass"});if(globalBlend<0||globalBlend>1)r.push_back({ValidationSeverity::Error,"globalBlend","Blend must be in [0,1]"});return r;}
};}
