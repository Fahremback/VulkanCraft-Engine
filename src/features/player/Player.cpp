#include "Player.hpp"
#include "World.hpp"
#include "SoundEngine.hpp"
#include "TerrainGenerator.hpp"
#include <cmath>
#include <algorithm>

Player::Player() {
    position.y = static_cast<float>(TerrainGenerator::sample(position.x, position.z).height + 3);
    camera.position = get_eye_position();
}

bool Player::check_collision(const glm::vec3& targetPos, const World& world) const {
    float minX = targetPos.x - width / 2.0f;
    float maxX = targetPos.x + width / 2.0f - 0.001f;
    float minY = targetPos.y;
    float maxY = targetPos.y + height - 0.001f;
    float minZ = targetPos.z - width / 2.0f;
    float maxZ = targetPos.z + width / 2.0f - 0.001f;

    int minBlockX = static_cast<int>(std::floor(minX));
    int maxBlockX = static_cast<int>(std::floor(maxX));
    int minBlockY = static_cast<int>(std::floor(minY));
    int maxBlockY = static_cast<int>(std::floor(maxY));
    int minBlockZ = static_cast<int>(std::floor(minZ));
    int maxBlockZ = static_cast<int>(std::floor(maxZ));

    for (int bx = minBlockX; bx <= maxBlockX; bx++) {
        for (int by = minBlockY; by <= maxBlockY; by++) {
            for (int bz = minBlockZ; bz <= maxBlockZ; bz++) {
                // A.2: registry-driven solidity — as_builtin_block mapped every
                // dynamic (registry-defined) block to Air, so collision never
                // saw JSON-defined blocks. is_solid_block_id resolves builtin
                // and dynamic blocks alike from the runtime table.
                if (world.is_solid_block_id(world.get_block_at(
                        glm::vec3(bx + 0.5f, by + 0.5f, bz + 0.5f)))) {
                    return true;
                }
            }
        }
    }
    return false;
}

void Player::move_with_collisions(const glm::vec3& displacement, const World& world, bool allowLedgeAssist) {
    const float longestAxis = (std::max)({ std::abs(displacement.x), std::abs(displacement.y), std::abs(displacement.z) });
    const int steps = (std::max)(1, static_cast<int>(std::ceil(longestAxis / 0.18f)));
    const glm::vec3 step = displacement / static_cast<float>(steps);

    bool usedLedgeAssist = false;
    auto moveHorizontal = [&](const glm::vec3& axisStep, float& axisPosition, float& axisVelocity) {
        const glm::vec3 candidate = position + axisStep;
        if (world.is_chunk_loaded_at(candidate) && !check_collision(candidate, world)) {
            axisPosition += axisStep.x + axisStep.z;
            return;
        }

        // A mesma colisão sólida continua valendo dentro e fora da água. A única
        // assistência de natação é procurar espaço livre logo acima da margem.
        if (allowLedgeAssist && !usedLedgeAssist) {
            for (int sample = 1; sample <= 11; ++sample) {
                const float lift = static_cast<float>(sample) * 0.1f;
                const glm::vec3 raisedCandidate = candidate + glm::vec3(0.0f, lift, 0.0f);
                if (world.is_chunk_loaded_at(raisedCandidate) && !check_collision(raisedCandidate, world)) {
                    position = raisedCandidate;
                    velocity.y = (std::max)(velocity.y, 2.4f);
                    isGrounded = false;
                    usedLedgeAssist = true;
                    return;
                }
            }
        }
        axisVelocity = 0.0f;
    };

    for (int iteration = 0; iteration < steps; ++iteration) {
        if (step.x != 0.0f) moveHorizontal(glm::vec3(step.x, 0.0f, 0.0f), position.x, velocity.x);

        if (step.z != 0.0f) moveHorizontal(glm::vec3(0.0f, 0.0f, step.z), position.z, velocity.z);

        const glm::vec3 candidateY = position + glm::vec3(0.0f, step.y, 0.0f);
        if (!check_collision(candidateY, world)) {
            position.y = candidateY.y;
            if (step.y != 0.0f) isGrounded = false;
        } else {
            if (step.y < 0.0f) isGrounded = true;
            velocity.y = 0.0f;
        }
    }
}

RaycastResult Player::perform_raycast(const World& world, float maxDistance) const {
    glm::vec3 rayOrigin = get_eye_position();
    glm::vec3 rayDir = camera.front;

    float step = 0.05f;
    glm::vec3 currentPos = rayOrigin;
    glm::vec3 lastAirPos = currentPos;

    for (float t = 0.0f; t < maxDistance; t += step) {
        currentPos = rayOrigin + rayDir * t;
        // A.2: registry-driven solidity (see check_collision) — dynamic blocks
        // now stop the ray too.
        if (world.is_solid_block_id(world.get_block_at(currentPos))) {
            return { true, currentPos, lastAirPos };
        }
        lastAirPos = currentPos;
    }

    return { false, glm::vec3(0.0f), glm::vec3(0.0f) };
}

void Player::update(float deltaTime, const PlayerInput& input, const World& world, SoundEngine& audio) {
    const float physicsDelta = glm::clamp(deltaTime, 0.0f, 0.05f);
    if (input.selectedBlock) selectedBlock = *input.selectedBlock;

    if (isSwinging) {
        swingProgress += physicsDelta * 5.0f;
        if (swingProgress >= 1.0f) {
            swingProgress = 0.0f;
            isSwinging = false;
        }
    }

    doubleTapTimer += physicsDelta;
    const bool spaceDown = input.jump;

    // Any data-driven fluid (META section 13): the rigid Water/Lava pair is
    // replaced by the world's fluid table (a custom fluid is swimmable too).
    const auto isFluidAt = [&](const glm::vec3& samplePosition) {
        return world.is_fluid_block_at(samplePosition);
    };
    const bool feetInFluid = isFluidAt(position + glm::vec3(0.0f, 0.15f, 0.0f));
    const bool waistInFluid = isFluidAt(position + glm::vec3(0.0f, height * 0.55f, 0.0f));
    const bool eyesInFluid = isFluidAt(get_eye_position());
    const bool wasSubmerged = isSubmerged;
    isInFluid = feetInFluid || waistInFluid || eyesInFluid;
    isSubmerged = eyesInFluid;
    if (isSubmerged && !wasSubmerged) audio.play_splash_sound();

    if (spaceDown && !spaceWasPressed) {
        if (!isInFluid && doubleTapTimer < 0.25f) {
            isFlying = !isFlying;
            velocity = glm::vec3(0.0f);
            doubleTapTimer = 1.0f;
        } else {
            doubleTapTimer = 0.0f;
        }
    }

    if (!world.is_chunk_loaded_at(position)) {
        velocity = glm::vec3(0.0f);
        spaceWasPressed = spaceDown;
        return;
    }

    if (position.y < 0.0f) {
        position = glm::vec3(8.0f, 45.0f, 8.0f);
        velocity = glm::vec3(0.0f);
    }

    glm::vec3 forward(camera.front.x, 0.0f, camera.front.z);
    glm::vec3 right(camera.right.x, 0.0f, camera.right.z);
    if (glm::dot(forward, forward) > 0.0001f) forward = glm::normalize(forward);
    if (glm::dot(right, right) > 0.0001f) right = glm::normalize(right);
    glm::vec3 moveDir(0.0f);
    if (input.forward) moveDir += forward;
    if (input.backward) moveDir -= forward;
    if (input.left) moveDir -= right;
    if (input.right) moveDir += right;
    if (glm::dot(moveDir, moveDir) > 0.0001f) moveDir = glm::normalize(moveDir);

    const float horizontalSpeed = isFlying ? flySpeed : (isInFluid ? moveSpeed * 0.62f : moveSpeed);
    velocity.x = moveDir.x * horizontalSpeed;
    velocity.z = moveDir.z * horizontalSpeed;
    const float moving = glm::dot(moveDir, moveDir) > 0.0001f ? 1.0f : 0.0f;
    const float targetWalk = (!isFlying && isGrounded) ? moving : 0.0f;
    walkAmount += (targetWalk - walkAmount) * (1.0f - std::exp(-physicsDelta * 12.0f));
    walkCycle += physicsDelta * (isInFluid ? 5.0f : 10.5f) * moving;

    if (isFlying) {
        velocity.y = 0.0f;
        if (spaceDown) velocity.y += flySpeed;
        if (input.descend) velocity.y -= flySpeed;
        isGrounded = false;
    } else if (isInFluid) {
        // Enquanto qualquer parte do corpo ainda está na água, Space mantém o
        // impulso. Ao pôr a cabeça para fora ele aumenta para vencer a margem.
        if (spaceDown) velocity.y = isSubmerged ? 5.4f : 7.2f;
        else if (input.descend) velocity.y = -4.5f;
        else velocity.y = (std::max)(velocity.y + gravity * 0.12f * physicsDelta, -2.0f);
    } else {
        if (isGrounded && spaceDown && !spaceWasPressed) {
            velocity.y = jumpStrength;
            isGrounded = false;
        }
        velocity.y += gravity * physicsDelta;
        if (glm::dot(moveDir, moveDir) > 0.0001f && isGrounded) {
            footstepTimer += physicsDelta;
            if (footstepTimer >= 0.35f) {
                footstepTimer = 0.0f;
                audio.play_footstep_sound();
            }
        }
    }

    const bool swimmingTowardLedge = isInFluid && spaceDown && moving > 0.0f;
    move_with_collisions(velocity * physicsDelta, world, swimmingTowardLedge);

    spaceWasPressed = spaceDown;
    camera.position = get_eye_position();
}
