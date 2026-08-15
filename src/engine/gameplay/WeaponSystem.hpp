#pragma once
#include "../core/uuid/UUID.hpp"
#include <glm/glm.hpp>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Engine {
enum class FireMode{Single,Burst,Automatic};
struct WeaponDefinition{UUID id;std::string name;FireMode fireMode{FireMode::Single};uint32_t magazineSize{30};uint32_t reserveAmmo{90};uint32_t burstCount{3};float roundsPerMinute{600};float reloadSeconds{2};float damage{20};float range{100};float spreadDegrees{1};bool hitscan{true};};
struct WeaponHit{UUID entity;glm::vec3 position{0},normal{0,1,0};float distance{};float damage{};};
class WeaponRuntime final{
public:
 explicit WeaponRuntime(WeaponDefinition definition);
 void set_raycast(std::function<std::optional<WeaponHit>(const glm::vec3&,const glm::vec3&,float)> callback){raycast_=std::move(callback);}
 void set_projectile_spawn(std::function<void(const glm::vec3&,const glm::vec3&,float)> callback){spawn_=std::move(callback);}
 bool trigger_pressed(const glm::vec3&origin,const glm::vec3&direction);void trigger_released()noexcept{held_=false;}void update(float deltaTime,const glm::vec3&origin,const glm::vec3&direction);
 bool reload();uint32_t ammo()const noexcept{return ammo_;}uint32_t reserve()const noexcept{return reserve_;}bool reloading()const noexcept{return reloadRemaining_>0;}const std::vector<WeaponHit>& hits()const noexcept{return hits_;}void clear_hits(){hits_.clear();}
private:bool fire(const glm::vec3&,const glm::vec3&);glm::vec3 spread(glm::vec3 direction);
 WeaponDefinition definition_;uint32_t ammo_{},reserve_{},burstRemaining_{};float cooldown_{},reloadRemaining_{};bool held_{};uint32_t randomState_{0x12345678};std::function<std::optional<WeaponHit>(const glm::vec3&,const glm::vec3&,float)>raycast_;std::function<void(const glm::vec3&,const glm::vec3&,float)>spawn_;std::vector<WeaponHit>hits_;
};
} // namespace Engine
