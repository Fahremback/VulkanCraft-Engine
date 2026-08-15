#pragma once
#include "EditorToolModel.hpp"
namespace Engine::Editor {
struct DestructionLevelModel{std::string name;uint32_t chunkCount{};float damageThreshold{10};float impulseScale{1};};
struct DestructionBondModel{uint32_t a{},b{};float strength{100};};
class DestructionEditorModel final:public EditorDocumentModel{public:UUID sourceMesh{0,0};uint32_t seed{1};std::vector<DestructionLevelModel>levels;std::vector<DestructionBondModel>bonds;
 void add_level(DestructionLevelModel l){levels.push_back(std::move(l));changed();}void regenerate(uint32_t s){seed=s;changed();}
 std::vector<ValidationIssue>validate()const override{std::vector<ValidationIssue>r;if(!sourceMesh.is_valid())r.push_back({ValidationSeverity::Error,"sourceMesh","Source mesh is required"});for(auto&l:levels)if(l.chunkCount==0||l.damageThreshold<0)r.push_back({ValidationSeverity::Error,l.name,"Invalid fracture level"});return r;}
};}
