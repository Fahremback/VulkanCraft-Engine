#pragma once
#include "EditorToolModel.hpp"
namespace Engine::Editor {
enum class TimelineTrackType:uint8_t{Animation,Audio,Event,Camera,Property};
struct TimelineKeyModel{float time{};std::string value;};
struct TimelineTrackModel{std::string name;TimelineTrackType type{};bool muted{};std::vector<TimelineKeyModel> keys;};
class TimelineEditorModel final:public EditorDocumentModel{public:float duration{1};float playhead{};bool loop{};std::vector<TimelineTrackModel> tracks;
 size_t add_track(TimelineTrackModel t){tracks.push_back(std::move(t));changed();return tracks.size()-1;}
 bool add_key(size_t track,TimelineKeyModel key){if(track>=tracks.size()||key.time<0||key.time>duration)return false;auto&k=tracks[track].keys;k.push_back(std::move(key));std::sort(k.begin(),k.end(),[](auto&a,auto&b){return a.time<b.time;});changed();return true;}
 void seek(float t){playhead=std::clamp(t,0.0f,duration);}
 std::vector<ValidationIssue> validate()const override{std::vector<ValidationIssue>r;if(duration<=0)r.push_back({ValidationSeverity::Error,"duration","Duration must be positive"});for(auto&t:tracks)for(auto&k:t.keys)if(k.time<0||k.time>duration)r.push_back({ValidationSeverity::Error,t.name,"Key lies outside timeline"});return r;}
};}
