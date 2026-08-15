#include "WeaponSystem.hpp"
#include <algorithm>
#include <cmath>
#include <glm/gtc/constants.hpp>

namespace Engine{
WeaponRuntime::WeaponRuntime(WeaponDefinition d):definition_(std::move(d)),ammo_(definition_.magazineSize),reserve_(definition_.reserveAmmo){}
glm::vec3 WeaponRuntime::spread(glm::vec3 d){d=glm::length(d)>0?glm::normalize(d):glm::vec3(0,0,-1);randomState_=1664525u*randomState_+1013904223u;float a=(randomState_&0xffff)/65535.0f*2-1;randomState_=1664525u*randomState_+1013904223u;float b=(randomState_&0xffff)/65535.0f*2-1;float radius=std::tan(glm::radians(definition_.spreadDegrees));glm::vec3 up=std::abs(d.y)<0.99f?glm::vec3(0,1,0):glm::vec3(1,0,0),right=glm::normalize(glm::cross(d,up));up=glm::normalize(glm::cross(right,d));return glm::normalize(d+right*a*radius+up*b*radius);}
bool WeaponRuntime::fire(const glm::vec3&o,const glm::vec3&d){if(ammo_==0||cooldown_>0||reloadRemaining_>0)return false;--ammo_;cooldown_=60.0f/std::max(definition_.roundsPerMinute,1.0f);glm::vec3 shot=spread(d);if(definition_.hitscan&&raycast_){auto hit=raycast_(o,shot,definition_.range);if(hit){hit->damage=definition_.damage;hits_.push_back(*hit);}}else if(spawn_)spawn_(o,shot,definition_.damage);return true;}
bool WeaponRuntime::trigger_pressed(const glm::vec3&o,const glm::vec3&d){held_=true;if(definition_.fireMode==FireMode::Burst)burstRemaining_=definition_.burstCount;bool fired=fire(o,d);if(fired&&burstRemaining_)--burstRemaining_;return fired;}
void WeaponRuntime::update(float dt,const glm::vec3&o,const glm::vec3&d){dt=std::max(dt,0.0f);cooldown_=std::max(0.0f,cooldown_-dt);if(reloadRemaining_>0){reloadRemaining_-=dt;if(reloadRemaining_<=0){uint32_t need=definition_.magazineSize-ammo_,take=std::min(need,reserve_);ammo_+=take;reserve_-=take;reloadRemaining_=0;}return;}if(ammo_==0&&reserve_>0){reload();return;}if(definition_.fireMode==FireMode::Automatic&&held_)fire(o,d);else if(definition_.fireMode==FireMode::Burst&&burstRemaining_&&fire(o,d))--burstRemaining_;}
bool WeaponRuntime::reload(){if(reloadRemaining_>0||ammo_>=definition_.magazineSize||reserve_==0)return false;reloadRemaining_=std::max(definition_.reloadSeconds,0.001f);held_=false;burstRemaining_=0;return true;}
} // namespace Engine
