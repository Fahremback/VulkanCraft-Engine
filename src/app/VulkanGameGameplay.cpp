#include "VulkanGame.hpp"
#include "VulkanGameSupport.hpp"

void VulkanGame::setupGameplay(){
        const char* candidates[] = { "Content/Scenes/Initial.script", "Content/Initial.script" };
        bool loadedFile = false;
        for (const char* candidate : candidates) {
            if (gameplayGraph.load(candidate)) { loadedFile = true; break; }
        }
        if (!loadedFile) {
            gameplayGraph.name = "PlayerController";
            const auto constant = [&](double value) {
                return TypedScriptNode{ UUID(), ScriptNodeKind::ConstantFloat, "", "", value };
            };
            const auto setVar = [&](const std::string& var) {
                return TypedScriptNode{ UUID(), ScriptNodeKind::SetVariable, "", var };
            };
            const auto getVar = [&](const std::string& var) {
                return TypedScriptNode{ UUID(), ScriptNodeKind::GetVariable, "", var };
            };
            gameplayGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::Event, "OnStart" });
            for (const auto& init : { std::pair{ std::string("playerX"), 0.0 }, { std::string("playerY"), 0.0 }, { std::string("playerZ"), 0.0 } }) {
                gameplayGraph.nodes.push_back(constant(init.second));
                gameplayGraph.nodes.push_back(setVar(init.first));
            }
            gameplayGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::Event, "Tick" });
            // playerX += moveX
            gameplayGraph.nodes.push_back(getVar("playerX"));
            gameplayGraph.nodes.push_back(getVar("moveX"));
            gameplayGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::AddFloat });
            gameplayGraph.nodes.push_back(setVar("playerX"));
            // playerZ += moveY
            gameplayGraph.nodes.push_back(getVar("playerZ"));
            gameplayGraph.nodes.push_back(getVar("moveY"));
            gameplayGraph.nodes.push_back(TypedScriptNode{ UUID(), ScriptNodeKind::AddFloat });
            gameplayGraph.nodes.push_back(setVar("playerZ"));
        }
        const auto compiled = ScriptCompiler::compile(gameplayGraph);
        if (!compiled) return;
        scriptVM.load(std::move(compiled.program));
        scriptVM.start_event("OnStart");
        gameplayLoaded = true;
        std::cout << "[Game] Player controller script " << (loadedFile ? "(authored)" : "(built-in)")
                  << " loaded, " << gameplayGraph.nodes.size() << " nodes\n";
    }

void VulkanGame::updateGameplay(float dt){
        if (!gameplayLoaded) return;
        const float speed = 4.0f;
        const float dx = ((glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS ? 1.0f : 0.0f) -
                          (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS ? 1.0f : 0.0f)) * speed * dt;
        const float dz = ((glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS ? 1.0f : 0.0f) -
                          (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS ? 1.0f : 0.0f)) * speed * dt;
        scriptVM.set_variable("moveX", dx);
        scriptVM.set_variable("moveY", dz);
        scriptVM.start_event("Tick");
        scriptVM.run(dt, 256);
        std::vector<std::string> emitted;
        scriptVM.consume_emitted_events(emitted);
        for (const std::string& event : emitted) scriptVM.start_event(event);
        const auto tit = scene.transformComponents.find(playerEntityId);
        if (tit != scene.transformComponents.end()) {
            tit->second.position = glm::vec3(
                static_cast<float>(scriptVM.float_variable("playerX")),
                static_cast<float>(scriptVM.float_variable("playerY")),
                static_cast<float>(scriptVM.float_variable("playerZ")));
        }
        if (!gameplayStatusLogged) {
            gameplayStatusLogged = true;
            std::cout << "[Game] Player controller via script active (WASD -> moveX/moveY -> playerX/playerZ)\n";
        }
    }

void VulkanGame::setupWeapon(){
        // The weapon is hitscan: each shot becomes a raycast against the same
        // physics world that steps rigidbodies/ragdolls in this frame.
        weapon.set_raycast([this](const glm::vec3& origin, const glm::vec3& dir, float maxDist)
                               -> std::optional<Engine::WeaponHit> {
            const auto hit = physics.raycast(origin, dir, maxDist);
            if (!hit) return std::nullopt;
            Engine::WeaponHit out;
            out.position = hit->point;
            out.normal = hit->normal;
            out.distance = hit->distance;
            lastHitBody = hit->body;
            lastHitPoint = hit->point;
            return out;
        });
        // Muzzle flash: a transient point light at the camera, lit for ~80ms
        // per shot by updateWeapon (intensity 0 keeps it out of the frame).
        auto muzzle = scene.create_entity("MuzzleFlash");
        muzzleLightEntity = muzzle.get_id();
        scene.lightComponents[muzzleLightEntity] = LightComponent{
            glm::vec3(1.0f, 0.75f, 0.35f), 0.0f, 12.0f, false, LightType::Point };
        scene.transformComponents[muzzleLightEntity].position = camPos;
    }

void VulkanGame::updateWeapon(float dt){
        const glm::vec3 front = cameraFront();
        const bool wasHeld = mouseLeftPrev;
        mouseLeftPrev = mouseLeftHeld;
        const bool clicked = mouseLeftHeld && !wasHeld;
        const uint32_t ammoBefore = weapon.ammo();
        if (clicked) weapon.trigger_pressed(camPos, front);
        if (!mouseLeftHeld) weapon.trigger_released();
        if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) weapon.reload();
        weapon.update(dt, camPos, front);
        const bool fired = weapon.ammo() < ammoBefore;

        // Muzzle flash light: position at the muzzle, decay over ~80ms.
        muzzleFlashTimer = std::max(0.0f, muzzleFlashTimer - dt);
        if (fired) muzzleFlashTimer = 0.08f;
        const auto lit = scene.lightComponents.find(muzzleLightEntity);
        if (lit != scene.lightComponents.end()) {
            lit->second.intensity = muzzleFlashTimer > 0.0f
                ? 900.0f * (muzzleFlashTimer / 0.08f) : 0.0f;
            scene.transformComponents[muzzleLightEntity].position = camPos + front * 0.6f;
        }

        // Feed the gameplay script and dispatch weapon events (OnShoot when a
        // shot leaves the barrel, OnHit when the hitscan ray finds a body).
        scriptVM.set_variable("ammo", static_cast<double>(weapon.ammo()));
        scriptVM.set_variable("reserve", static_cast<double>(weapon.reserve()));
        scriptVM.set_variable("reloading", weapon.reloading() ? 1.0 : 0.0);
        std::vector<std::string> events;
        if (fired) events.push_back("OnShoot");
        const auto& hits = weapon.hits();
        if (hits.size() > weaponHitsSeen) {
            weaponHitsSeen = static_cast<uint32_t>(hits.size());
            const Engine::WeaponHit& h = hits.back();
            scriptVM.set_variable("hitDistance", static_cast<double>(h.distance));
            events.push_back("OnHit");
            if (!weaponHitLogged) {
                weaponHitLogged = true;
                std::cout << "[Game] Weapon hitscan raycast hit at ("
                          << h.position.x << ", " << h.position.y << ", "
                          << h.position.z << ") dist=" << h.distance
                          << " dmg=" << h.damage << "\n";
            }
            // FPS targets: knock the crate that was hit; 3 hits destroy it.
            for (FpsTarget& target : fpsTargets) {
                if (!target.alive || target.body != lastHitBody) continue;
                ++target.hits;
                physics.apply_impulse_at_point(target.body,
                    glm::normalize(front) * 7.0f + glm::vec3(0.0f, 1.8f, 0.0f), lastHitPoint);
                if (target.hits >= 3) {
                    target.alive = false;
                    physics.destroy_body(target.body);
                    const auto hitMesh = scene.meshRendererComponents.find(target.entity);
                    if (hitMesh != scene.meshRendererComponents.end()) hitMesh->second.isVisible = false;
                    ++targetsDestroyed;
                    std::cout << "[Game] Target crate destroyed — "
                              << (static_cast<int>(fpsTargets.size()) - targetsDestroyed)
                              << " left\n";
                }
                break;
            }
        }
        for (const std::string& ev : events) {
            scriptVM.start_event(ev);
            scriptVM.run(dt, 256);
            std::vector<std::string> emitted;
            scriptVM.consume_emitted_events(emitted);
            for (const std::string& e : emitted) scriptVM.start_event(e);
        }
        if (fired && !weaponStatusLogged) {
            weaponStatusLogged = true;
            std::cout << "[Game] Weapon 'Assault Rifle' firing via physics raycast"
                      << " (auto, dmg 25, spread 1.5deg, hitscan)\n";
        }
    }

void VulkanGame::update(float dt){
        physics.step(dt);
        updateGameplay(dt);
        updateWeapon(dt);
        updateSunShadow();
        skinnedAnimTime += dt;

        // Ragdoll: toss the flag once it has settled, and log the Tip position
        // at 2s so headless runs prove the physics pose actually moved.
        if (ragdollBuilt && !ragdollImpulseApplied && skinnedAnimTime > 0.6f) {
            ragdoll.apply_impulse(physics, "Tip", glm::vec3(3.0f, 2.5f, 0.0f));
            ragdollImpulseApplied = true;
            std::cout << "[Game] Ragdoll impulse applied to Tip\n";
        }
        if (ragdollBuilt && !ragdollTipLogged && skinnedAnimTime >= 2.0f) {
            for (const Physics::RagdollPoseBone& bone : ragdoll.pose(physics)) {
                if (bone.name == "Tip") {
                    std::cout << "[Game] Ragdoll Tip pos after 2s: (" << bone.position.x
                              << ", " << bone.position.y << ", " << bone.position.z << ")\n";
                    ragdollTipLogged = true;
                    break;
                }
            }
        }

        // Free-fly camera.
        const float baseSpeed = 6.0f;
        const float speed = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 3.0f : 1.0f) * baseSpeed * dt;
        const glm::vec3 front = cameraFront();
        const glm::vec3 right = glm::normalize(glm::cross(front, glm::vec3(0, 1, 0)));
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) camPos += front * speed;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) camPos -= front * speed;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) camPos += right * speed;
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) camPos -= right * speed;
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) camPos.y += speed;
        if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) camPos.y -= speed;

        // F9 toggles captured-cursor mouse-look (right-drag look fallback).
        const bool f9Down = glfwGetKey(window, GLFW_KEY_F9) == GLFW_PRESS;
        if (f9Down && !f9WasPressed) {
            mouseLookEnabled = !mouseLookEnabled;
            glfwSetInputMode(window, GLFW_CURSOR,
                mouseLookEnabled ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        }
        f9WasPressed = f9Down;
    }

