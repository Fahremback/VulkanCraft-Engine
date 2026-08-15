#pragma once
#include "EditorToolModel.hpp"
namespace Engine::Editor {
struct WheelModel{std::string name;glm::vec3 position{0};float radius{0.45f},width{0.25f},suspensionTravel{0.3f};bool steering{},driven{},braking{true};};
struct VehicleSeatModel{std::string name;glm::vec3 position{0};bool driver{};};
class VehicleEditorModel final:public EditorDocumentModel{public:std::string name;float mass{1500},horsepower{300},maxRpm{6500},drag{0.3f};std::vector<WheelModel>wheels;std::vector<VehicleSeatModel>seats;
 void add_wheel(WheelModel w){wheels.push_back(std::move(w));changed();}void add_seat(VehicleSeatModel s){seats.push_back(std::move(s));changed();}
 std::vector<ValidationIssue>validate()const override{std::vector<ValidationIssue>r;if(mass<=0||horsepower<0||maxRpm<=0)r.push_back({ValidationSeverity::Error,"powertrain","Invalid vehicle parameters"});if(wheels.size()<2)r.push_back({ValidationSeverity::Warning,"wheels","Vehicle normally needs at least two wheels"});if(std::count_if(seats.begin(),seats.end(),[](auto&s){return s.driver;})>1)r.push_back({ValidationSeverity::Error,"seats","Only one driver seat is allowed"});return r;}
};}
