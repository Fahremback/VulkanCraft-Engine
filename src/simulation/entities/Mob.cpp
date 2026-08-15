#include "Mob.hpp"
#include "World.hpp"
#include "SoundEngine.hpp"
#include "TerrainGenerator.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <iostream>
#include <algorithm>

Mob::Mob(MobType mobType, glm::vec3 spawnPos, SoundEngine& audio)
    : type(mobType), position(spawnPos), audio_(&audio) {
    switch (type) {
    case MobType::Zombie: health = maxHealth = 20.0f; break;
    case MobType::Skeleton: health = maxHealth = 20.0f; break;
    case MobType::Creeper: health = maxHealth = 20.0f; break;
    case MobType::Cow: health = maxHealth = 10.0f; break;
    case MobType::Pig: health = maxHealth = 10.0f; break;
    case MobType::Sheep: health = maxHealth = 8.0f; break;
    }
}

void Mob::take_damage(float damage, const glm::vec3& knockbackDir) {
    if (!isAlive) return;
    health -= damage;
    velocity += knockbackDir * 6.0f;
    hurtTimer = 0.4f;

    if (health <= 0.0f) {
        isAlive = false;
        audio_->play_sound("player_hurt");
    }
}

void Mob::update(float dt, const glm::vec3& playerPos, World& world) {
    if (!isAlive) return;

    if (hurtTimer > 0.0f) hurtTimer -= dt;

    float distToPlayer = glm::distance(position, playerPos);

    // 1. Queima ao Sol na Superfície
    if ((type == MobType::Zombie || type == MobType::Skeleton) && position.y > TerrainGenerator::SeaLevel) {
        BlockType above = as_builtin_block(world.get_block_at(glm::ivec3(position.x, position.y + 2.0f, position.z)));
        if (above == BlockType::Air) {
            take_damage(3.0f * dt, glm::vec3(0.0f));
        }
    }

    // 1b. Dano por fluido (META section 13): o mob dentro de um fluido com
    // damagePerTick (ex.: lava) toma o dano data-driven por segundo.
    if (const FluidParams* fluid = world.fluid_params_at(
            glm::ivec3(position.x, position.y + 0.2f, position.z))) {
        if (fluid->damagePerTick > 0.0f) {
            take_damage(fluid->damagePerTick * dt, glm::vec3(0.0f));
        }
    }

    // 2. IA do Creeper (Detonação)
    if (type == MobType::Creeper && distToPlayer < 3.5f) {
        state = MobAIState::Exploding;
        creeperFuseTimer += dt * 1.5f;
        if (creeperFuseTimer >= 1.5f) {
            audio_->play_sound("explosion");
            isAlive = false;
            // Destruição de terreno por explosão
            for (int x = -2; x <= 2; ++x) {
                for (int y = -2; y <= 2; ++y) {
                    for (int z = -2; z <= 2; ++z) {
                        glm::ivec3 targetPos = glm::ivec3(position) + glm::ivec3(x, y, z);
                        if (as_builtin_block(world.get_block_at(targetPos)) != BlockType::Bedrock) {
                            world.set_block_at(targetPos, runtime_id(BlockType::Air));
                        }
                    }
                }
            }
            return;
        }
    } else if (type == MobType::Creeper && state == MobAIState::Exploding) {
        creeperFuseTimer = std::max(0.0f, creeperFuseTimer - dt);
        if (creeperFuseTimer == 0.0f) state = MobAIState::Idle;
    }

    // 3. Perseguição / Patrulha de IA
    aiTimer -= dt;
    if (aiTimer <= 0.0f) {
        aiTimer = 2.0f + (rand() % 100) / 50.0f;

        if (distToPlayer < 16.0f && (type == MobType::Zombie || type == MobType::Skeleton || type == MobType::Creeper)) {
            state = MobAIState::ChasePlayer;
        } else {
            state = (rand() % 3 == 0) ? MobAIState::Idle : MobAIState::Wander;
            if (state == MobAIState::Wander) {
                yaw = (rand() % 360) * 0.0174533f;
            }
        }
    }

    glm::vec3 moveDir(0.0f);
    float speed = 1.8f;

    if (state == MobAIState::ChasePlayer) {
        glm::vec3 dir = playerPos - position;
        dir.y = 0.0f;
        if (glm::length(dir) > 0.1f) {
            moveDir = glm::normalize(dir);
            yaw = std::atan2(-moveDir.x, -moveDir.z);
            speed = 3.2f;
        }
    } else if (state == MobAIState::Wander) {
        moveDir = glm::vec3(-std::sin(yaw), 0.0f, -std::cos(yaw));
    }

    velocity.x = moveDir.x * speed;
    velocity.z = moveDir.z * speed;
    velocity.y -= 18.0f * dt; // Gravidade

    // Movimentação & Animação
    position += velocity * dt;
    float horizSpeed = glm::length(glm::vec3(velocity.x, 0.0f, velocity.z));
    walkAnimProgress += horizSpeed * dt * 4.0f;

    // Colisão simples com terreno: fluidos (água, lava ou data-driven — META
    // §13) nunca são chão; o mob afunda neles até o fundo sólido.
    glm::ivec3 blockBelow(position.x, position.y - 0.1f, position.z);
    if (!world.is_fluid_block_at(glm::vec3(blockBelow)) &&
        as_builtin_block(world.get_block_at(blockBelow)) != BlockType::Air) {
        position.y = std::ceil(position.y - 0.1f);
        velocity.y = 0.0f;
    }
}

void MobManager::spawn_mob(MobType type, glm::vec3 pos) {
    mobs.emplace_back(type, pos, audio_);
}

void MobManager::update(float dt, const glm::vec3& playerPos, World& world) {
    for (auto& mob : mobs) {
        mob.update(dt, playerPos, world);
    }

    // Remover Mobs Mortos
    mobs.erase(std::remove_if(mobs.begin(), mobs.end(), [](const Mob& m) { return !m.isAlive; }), mobs.end());
}


