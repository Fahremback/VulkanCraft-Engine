#pragma once
#include "EditorToolModel.hpp"
namespace Engine::Editor {
enum class IKSolverKind:uint8_t{TwoBone,CCD,FABRIK,LookAt};
struct IKChainModel{std::string name;std::string rootBone;std::string endBone;IKSolverKind solver{IKSolverKind::TwoBone};uint32_t iterations{8};float weight{1};glm::vec3 pole{0,1,0};};
class IKEditorModel final:public EditorDocumentModel{public:std::vector<IKChainModel> chains;std::optional<size_t> selected;
 bool add_chain(IKChainModel c){if(c.name.empty()||std::any_of(chains.begin(),chains.end(),[&](auto&x){return x.name==c.name;}))return false;chains.push_back(std::move(c));changed();return true;}
 bool remove_chain(size_t i){if(!erase_index(chains,i))return false;selected.reset();changed();return true;}
 std::vector<ValidationIssue> validate()const override{std::vector<ValidationIssue>r;for(auto&c:chains){if(c.rootBone.empty()||c.endBone.empty())r.push_back({ValidationSeverity::Error,c.name,"Root and end bones are required"});if(c.iterations==0||c.weight<0||c.weight>1)r.push_back({ValidationSeverity::Error,c.name,"Invalid solver settings"});}return r;}
};}
