#pragma once
#include "../PluginContract.hpp"
#include <glm/glm.hpp>
namespace Engine::Plugins {
struct VehicleControl{float throttle{},brake{},steering{},handbrake{};};
struct VehicleTelemetry{glm::vec3 position{0},linearVelocity{0};float engineRpm{},speedMetersPerSecond{};bool grounded{};};
class IVehicleSimulation{public:virtual~IVehicleSimulation()=default;virtual uint64_t spawn(UUID asset,const glm::vec3&position)=0;virtual void control(uint64_t vehicle,const VehicleControl&input)=0;virtual VehicleTelemetry telemetry(uint64_t vehicle)const=0;virtual void despawn(uint64_t vehicle)=0;};
class VehiclesPlugin final:public EnginePlugin{public:explicit VehiclesPlugin(std::shared_ptr<IVehicleSimulation> service={}):service_(std::move(service)){}std::string get_name()const override{return "Vehicles";}std::string get_version()const override{return "1.0.0";}IVehicleSimulation* service()const noexcept{return service_.get();}
protected:void register_types(TypeRegistry&)override;void register_assets(AssetRegistry&)override;void register_editor_tools(Editor::EditorRegistry&)override;
private:std::shared_ptr<IVehicleSimulation>service_;};
} // namespace Engine::Plugins
