#pragma once

#include "Camera.hpp"
#include "Voxel.hpp"
#include "PlayerInput.hpp"
#include <glm/glm.hpp>

class World;
class SoundEngine;

struct RaycastResult {
    bool hit{ false };
    glm::vec3 hitBlockPos{ 0.0f };
    glm::vec3 placeBlockPos{ 0.0f };
};

class Player {
public:
    glm::vec3 position{ 8.0f, 45.0f, 8.0f };
    glm::vec3 velocity{ 0.0f };

    float width{ 0.6f };
    float height{ 1.8f };
    float eyeHeight{ 1.62f };

    float moveSpeed{ 8.0f };
    float flySpeed{ 30.0f };
    float jumpStrength{ 9.5f };
    float gravity{ -28.0f };
    bool isGrounded{ false };
    bool isFlying{ false };
    bool isSubmerged{ false };
    bool isInFluid{ false };

    BlockType selectedBlock{ BlockType::Dirt };

    float doubleTapTimer{ 1.0f };
    bool spaceWasPressed{ false };

    // Animação de Soco do Braço
    float swingProgress{ 0.0f };
    bool isSwinging{ false };

    float footstepTimer{ 0.0f };
    float walkCycle{ 0.0f };
    float walkAmount{ 0.0f };

    Camera camera;

    Player();

    void trigger_swing() { isSwinging = true; swingProgress = 0.0f; }
    void update(float deltaTime, const PlayerInput& input, const World& world, SoundEngine& audio);
    RaycastResult perform_raycast(const World& world, float maxDistance = 6.0f) const;
    glm::vec3 get_eye_position() const { return position + glm::vec3(0.0f, eyeHeight, 0.0f); }

private:
    bool check_collision(const glm::vec3& targetPos, const World& world) const;
    void move_with_collisions(const glm::vec3& displacement, const World& world, bool allowLedgeAssist);
};
