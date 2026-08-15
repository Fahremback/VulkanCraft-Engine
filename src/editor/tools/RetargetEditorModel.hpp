#pragma once
#include "EditorToolModel.hpp"
namespace Engine::Editor {
struct RetargetBoneMapModel{std::string sourceBone;std::string targetBone;float translationScale{1};glm::vec3 rotationOffset{0};};
class RetargetEditorModel final:public EditorDocumentModel{public:UUID sourceSkeleton{0,0},targetSkeleton{0,0};std::vector<RetargetBoneMapModel> mapping;bool preserveRootMotion{true};
 bool map(RetargetBoneMapModel m){if(m.sourceBone.empty()||m.targetBone.empty())return false;auto i=std::find_if(mapping.begin(),mapping.end(),[&](auto&x){return x.sourceBone==m.sourceBone;});if(i==mapping.end())mapping.push_back(std::move(m));else *i=std::move(m);changed();return true;}
 std::vector<ValidationIssue> validate()const override{std::vector<ValidationIssue>r;if(!sourceSkeleton.is_valid()||!targetSkeleton.is_valid())r.push_back({ValidationSeverity::Error,"skeleton","Source and target skeletons are required"});if(mapping.empty())r.push_back({ValidationSeverity::Warning,"mapping","No bones mapped"});return r;}
};}
