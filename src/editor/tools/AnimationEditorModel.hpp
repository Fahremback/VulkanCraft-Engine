#pragma once
#include "EditorToolModel.hpp"
#include <unordered_map>
namespace Engine::Editor {
struct AnimationStateModel { std::string id; UUID clip{0,0}; bool loop{true}; float speed{1.0f}; };
struct AnimationTransitionModel { std::string from; std::string to; std::string condition; float blendSeconds{0.2f}; };
class AnimationEditorModel final : public EditorDocumentModel {
public:
 std::vector<AnimationStateModel> states; std::vector<AnimationTransitionModel> transitions; std::string entryState; float previewTime{}; bool playing{};
 bool add_state(AnimationStateModel state){if(state.id.empty()||find(state.id))return false;states.push_back(std::move(state));changed();return true;}
 bool remove_state(const std::string& id){const auto n=std::erase_if(states,[&](const auto& s){return s.id==id;});if(!n)return false;std::erase_if(transitions,[&](const auto&t){return t.from==id||t.to==id;});if(entryState==id)entryState.clear();changed();return true;}
 AnimationStateModel* find(const std::string& id){auto i=std::find_if(states.begin(),states.end(),[&](auto&s){return s.id==id;});return i==states.end()?nullptr:&*i;}
 std::vector<ValidationIssue> validate()const override{std::vector<ValidationIssue> r;if(states.empty())r.push_back({ValidationSeverity::Warning,"states","Animation graph has no states"});if(!entryState.empty()&&std::none_of(states.begin(),states.end(),[&](auto&s){return s.id==entryState;}))r.push_back({ValidationSeverity::Error,"entryState","Entry state does not exist"});for(auto&t:transitions)if(t.blendSeconds<0)r.push_back({ValidationSeverity::Error,"transition","Blend duration cannot be negative"});return r;}
};
} // namespace Engine::Editor
