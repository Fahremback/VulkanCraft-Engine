#include "GameplayVisual.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>

namespace Engine {
MissionRuntime::MissionRuntime(MissionDefinition definition):definition_(std::move(definition)){}
void MissionRuntime::activate(){if(state_==MissionState::Inactive){state_=definition_.steps.empty()?MissionState::Completed:MissionState::Active;currentStep_=0;currentCount_=0;}}
void MissionRuntime::fail(){if(state_==MissionState::Active)state_=MissionState::Failed;}
bool MissionRuntime::signal(const std::string& signal){
    if(state_!=MissionState::Active||currentStep_>=definition_.steps.size()||definition_.steps[currentStep_].signal!=signal)return false;
    if(++currentCount_>=definition_.steps[currentStep_].requiredCount){++currentStep_;currentCount_=0;if(currentStep_>=definition_.steps.size())state_=MissionState::Completed;}return true;
}
bool MissionRuntime::save(const std::filesystem::path& path)const{std::ofstream out(path,std::ios::trunc);if(!out)return false;out<<"VulkanEngine.MissionState 1\n"<<std::quoted(definition_.id.to_string())<<' '<<static_cast<int>(state_)<<' '<<currentStep_<<' '<<currentCount_<<'\n';return out.good();}
bool MissionRuntime::load(const std::filesystem::path& path){std::ifstream in(path);std::string magic,id;unsigned version{};int state{};size_t step{};uint32_t count{};if(!(in>>magic>>version)||magic!="VulkanEngine.MissionState"||version!=1||!(in>>std::quoted(id)>>state>>step>>count))return false;if(UUID::from_string(id)!=definition_.id||state<0||state>static_cast<int>(MissionState::Failed)||step>definition_.steps.size())return false;state_=static_cast<MissionState>(state);currentStep_=step;currentCount_=count;return true;}
DialogueRuntime::DialogueRuntime(DialogueGraph graph):graph_(std::move(graph)),current_(graph_.entry){if(!find(current_))current_.clear();}
void DialogueRuntime::set_condition(std::string name,bool value){conditions_[std::move(name)]=value;}
const DialogueNode* DialogueRuntime::find(const std::string&id)const{auto i=std::find_if(graph_.nodes.begin(),graph_.nodes.end(),[&](const auto&n){return n.id==id;});return i==graph_.nodes.end()?nullptr:&*i;}
std::string DialogueRuntime::text()const{auto*n=find(current_);return n?n->text:std::string{};}
std::vector<DialogueChoice> DialogueRuntime::available_choices()const{std::vector<DialogueChoice> result;auto*n=find(current_);if(!n)return result;for(const auto&c:n->choices)if(c.condition.empty()||(conditions_.contains(c.condition)&&conditions_.at(c.condition)))result.push_back(c);return result;}
bool DialogueRuntime::choose(size_t index){auto choices=available_choices();if(index>=choices.size())return false;if(choices[index].target.empty()){current_.clear();return true;}if(!find(choices[index].target))return false;current_=choices[index].target;return true;}
bool TriggerVolume::contains(const glm::vec3&p)const{glm::vec3 d=glm::abs(p-center);return d.x<=halfExtents.x&&d.y<=halfExtents.y&&d.z<=halfExtents.z;}
void TriggerVolume::update(UUID entity,const glm::vec3&p,uint32_t layer){if((layerMask&layer)==0)return;bool now=contains(p),was=inside_.contains(entity);if(now&&!was){inside_.insert(entity);if(onEnter)onEnter(entity);}else if(now&&was){if(onStay)onStay(entity);}else if(!now&&was){inside_.erase(entity);if(onExit)onExit(entity);}}
bool InteractionSystem::register_interactable(Interactable i){if(!i.entity.is_valid()||!i.action||i.radius<0)return false;return interactables_.emplace(i.entity,std::move(i)).second;}
bool InteractionSystem::unregister_interactable(UUID id){return interactables_.erase(id)>0;}
std::vector<UUID> InteractionSystem::query(const glm::vec3&p)const{std::vector<std::pair<float,UUID>> found;for(const auto&[id,i]:interactables_)if(i.enabled){float d=glm::distance(i.position,p);if(d<=i.radius)found.push_back({d,id});}std::sort(found.begin(),found.end(),[](auto&a,auto&b){return a.first==b.first?a.second<b.second:a.first<b.first;});std::vector<UUID> result;for(auto&[d,id]:found)result.push_back(id);return result;}
bool InteractionSystem::interact(UUID instigator,const glm::vec3&p){auto candidates=query(p);if(candidates.empty())return false;auto i=interactables_.find(candidates.front());if(i==interactables_.end()||!i->second.action)return false;i->second.action(instigator);return true;}
bool QuestJournal::add(MissionDefinition d){if(!d.id.is_valid())return false;UUID id=d.id;return missions_.emplace(id,MissionRuntime(std::move(d))).second;}
MissionRuntime* QuestJournal::mission(UUID id){auto i=missions_.find(id);return i==missions_.end()?nullptr:&i->second;}
void QuestJournal::signal_all(const std::string&s){for(auto&[id,m]:missions_)m.signal(s);}
bool QuestJournal::save(const std::filesystem::path& path)const{std::error_code e;std::filesystem::create_directories(path,e);if(e)return false;for(const auto&[id,m]:missions_)if(!m.save(path/(id.to_string()+".missionstate")))return false;return true;}
} // namespace Engine
