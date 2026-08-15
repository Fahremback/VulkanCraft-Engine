#pragma once
#include "../PluginContract.hpp"
#include "../../engine/animation/AnimationRuntime.hpp"
namespace Engine::Plugins {
struct ProceduralAnimationRequest{UUID entity{0,0};Pose basePose;float deltaTime{};};
class IProceduralAnimationService{public:virtual~IProceduralAnimationService()=default;virtual Pose evaluate(ProceduralAnimationRequest request)=0;virtual void set_layer_enabled(UUID entity,std::string_view layer,bool enabled)=0;};
class ProceduralAnimationPlugin final:public EnginePlugin{public:explicit ProceduralAnimationPlugin(std::shared_ptr<IProceduralAnimationService>s={}):service_(std::move(s)){}std::string get_name()const override{return "ProceduralAnimation";}std::string get_version()const override{return "1.0.0";}IProceduralAnimationService*service()const noexcept{return service_.get();}protected:void register_types(TypeRegistry&)override;void register_assets(AssetRegistry&)override;void register_editor_tools(Editor::EditorRegistry&)override;private:std::shared_ptr<IProceduralAnimationService>service_;};}
