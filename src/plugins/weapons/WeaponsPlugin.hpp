#pragma once
#include "../PluginContract.hpp"
#include <glm/glm.hpp>
namespace Engine::Plugins {
struct WeaponFireRequest{uint64_t weapon{};UUID instigator{0,0};glm::vec3 origin{0},direction{0,0,1};};struct WeaponFireResult{bool fired{};uint32_t ammunitionRemaining{};std::vector<UUID>hits;};
class IWeaponSimulation{public:virtual~IWeaponSimulation()=default;virtual uint64_t equip(UUID asset,UUID owner)=0;virtual WeaponFireResult fire(const WeaponFireRequest&)=0;virtual bool reload(uint64_t weapon)=0;virtual void unequip(uint64_t weapon)=0;};
class WeaponsPlugin final:public EnginePlugin{public:explicit WeaponsPlugin(std::shared_ptr<IWeaponSimulation>s={}):service_(std::move(s)){}std::string get_name()const override{return "Weapons";}std::string get_version()const override{return "1.0.0";}IWeaponSimulation*service()const noexcept{return service_.get();}protected:void register_types(TypeRegistry&)override;void register_assets(AssetRegistry&)override;void register_editor_tools(Editor::EditorRegistry&)override;private:std::shared_ptr<IWeaponSimulation>service_;};}
