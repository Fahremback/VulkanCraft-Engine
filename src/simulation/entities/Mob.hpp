#pragma once

#include "Voxel.hpp"
#include <glm/glm.hpp>
#include <vector>

enum class MobType {
    Zombie,
    Skeleton,
    Creeper,
    Cow,
    Pig,
    Sheep
};

enum class MobAIState {
    Idle,
    Wander,
    ChasePlayer,
    Attack,
    Exploding
};

class World;
class SoundEngine;

class Mob {
public:
    MobType type;
    glm::vec3 position;
    glm::vec3 velocity{ 0.0f };
    float yaw{ 0.0f };
    float pitch{ 0.0f };

    float health{ 20.0f };
    float maxHealth{ 20.0f };
    bool isAlive{ true };

    MobAIState state{ MobAIState::Idle };
    float aiTimer{ 0.0f };
    float walkAnimProgress{ 0.0f };
    float creeperFuseTimer{ 0.0f };
    float hurtTimer{ 0.0f };

    Mob(MobType mobType, glm::vec3 spawnPos, SoundEngine& audio);

    void update(float dt, const glm::vec3& playerPos, World& world);
    void take_damage(float damage, const glm::vec3& knockbackDir);

private:
    SoundEngine* audio_;
};

class MobManager {
public:
    explicit MobManager(SoundEngine& audio) : audio_(audio) {}
    std::vector<Mob> mobs;

    void spawn_mob(MobType type, glm::vec3 pos);
    void update(float dt, const glm::vec3& playerPos, World& world);

private:
    SoundEngine& audio_;
};
