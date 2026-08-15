#pragma once
#include "../PluginContract.hpp"
#include <functional>
namespace Engine::Plugins {
enum class MissionStatus:uint8_t{Inactive,Active,Completed,Failed};struct MissionEvent{UUID mission{0,0};std::string signal;uint32_t count{1};};
class IMissionService{public:virtual~IMissionService()=default;virtual bool activate(UUID mission)=0;virtual bool publish(const MissionEvent&event)=0;virtual MissionStatus status(UUID mission)const=0;virtual void subscribe(std::function<void(UUID,MissionStatus)>callback)=0;};
class MissionsPlugin final:public EnginePlugin{public:explicit MissionsPlugin(std::shared_ptr<IMissionService>s={}):service_(std::move(s)){}std::string get_name()const override{return "Missions";}std::string get_version()const override{return "1.0.0";}IMissionService*service()const noexcept{return service_.get();}protected:void register_types(TypeRegistry&)override;void register_assets(AssetRegistry&)override;void register_editor_tools(Editor::EditorRegistry&)override;private:std::shared_ptr<IMissionService>service_;};}
