#pragma once
#include "EditorToolModel.hpp"
namespace Engine::Editor {
struct ParticleModuleModel{std::string type;bool enabled{true};std::vector<std::pair<std::string,float>>parameters;};
class ParticleEditorModel final:public EditorDocumentModel{public:float spawnRate{10},lifetime{1};uint32_t maxParticles{1000};bool looping{true},localSpace{};std::vector<ParticleModuleModel>modules;
 void add_module(ParticleModuleModel m){modules.push_back(std::move(m));changed();}bool remove_module(size_t i){if(!erase_index(modules,i))return false;changed();return true;}
 std::vector<ValidationIssue>validate()const override{std::vector<ValidationIssue>r;if(spawnRate<0||lifetime<=0||maxParticles==0)r.push_back({ValidationSeverity::Error,"emitter","Invalid emitter limits"});for(auto&m:modules)if(m.type.empty())r.push_back({ValidationSeverity::Error,"module","Module type is required"});return r;}
};}
