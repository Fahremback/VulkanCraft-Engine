#pragma once
#include "../PluginContract.hpp"
#include <glm/glm.hpp>
namespace Engine::Plugins {
struct AdvancedRaycastHit{UUID entity{0,0};glm::vec3 point{0},normal{0,1,0};float distance{};uint32_t material{};};struct ConstraintHandle{uint64_t value{};explicit operator bool()const{return value!=0;}};
class IAdvancedPhysicsService{public:virtual~IAdvancedPhysicsService()=default;virtual std::vector<AdvancedRaycastHit>raycast_all(const glm::vec3&origin,const glm::vec3&direction,float distance,uint32_t mask)=0;virtual ConstraintHandle create_constraint(UUID a,UUID b,std::string_view type)=0;virtual void destroy_constraint(ConstraintHandle)=0;virtual void apply_impulse(UUID entity,const glm::vec3&impulse,const glm::vec3&point)=0;};
class AdvancedPhysicsPlugin final:public EnginePlugin{public:explicit AdvancedPhysicsPlugin(std::shared_ptr<IAdvancedPhysicsService>s={}):service_(std::move(s)){}std::string get_name()const override{return "AdvancedPhysics";}std::string get_version()const override{return "1.0.0";}IAdvancedPhysicsService*service()const noexcept{return service_.get();}protected:void register_types(TypeRegistry&)override;void register_assets(AssetRegistry&)override;void register_editor_tools(Editor::EditorRegistry&)override;private:std::shared_ptr<IAdvancedPhysicsService>service_;};}
