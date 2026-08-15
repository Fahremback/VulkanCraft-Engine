#pragma once
#include "EditorToolModel.hpp"
namespace Engine::Editor {
struct RecoilPointModel{float time{};glm::vec2 offset{0};};
class WeaponEditorModel final:public EditorDocumentModel{public:std::string name;float damage{35},roundsPerMinute{600},muzzleVelocity{800},reloadSeconds{2};uint32_t magazine{30},burstCount{3};bool automatic{true};UUID mesh{0,0},fireAudio{0,0};std::vector<RecoilPointModel>recoil;
 float seconds_per_shot()const{return roundsPerMinute>0?60.0f/roundsPerMinute:0;}void add_recoil(RecoilPointModel p){recoil.push_back(p);std::sort(recoil.begin(),recoil.end(),[](auto&a,auto&b){return a.time<b.time;});changed();}
 std::vector<ValidationIssue>validate()const override{std::vector<ValidationIssue>r;if(name.empty())r.push_back({ValidationSeverity::Error,"name","Weapon name is required"});if(damage<0||roundsPerMinute<=0||magazine==0||reloadSeconds<0)r.push_back({ValidationSeverity::Error,"ballistics","Invalid weapon parameters"});return r;}
};}
