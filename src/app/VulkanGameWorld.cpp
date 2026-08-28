#include "VulkanGame.hpp"
#include "VulkanGameSupport.hpp"

void VulkanGame::initScene(){
        // The packaged game ships Content/Scenes/Initial.scene (written by the
        // editor's Build Game step). Development fallbacks: the editor's active
        // scene file, then a small built-in scene.
        bool loaded = false;
        for (const char* candidate : { "Content/Scenes/Initial.scene", "assets/scenes/active_world.scene" }) {
            if (scene.load_from_file(candidate)) {
                std::cout << "[Game] Loaded scene: " << candidate << '\n';
                loaded = true;
                break;
            }
        }
        if (!loaded) {
            std::cout << "[Game] No scene file found; using built-in scene\n";
            auto cam = scene.create_entity("Camera");
            scene.cameraComponents[cam.get_id()] = CameraComponent{};
            auto transform = TransformComponent{};
            transform.position = glm::vec3(0.0f, 1.0f, -6.0f);
            scene.transformComponents[cam.get_id()] = transform;

            auto cube = scene.create_entity("Player");
            scene.meshRendererComponents[cube.get_id()] = MeshRendererComponent{};
            scene.materialComponents[cube.get_id()] = MaterialComponent{
                glm::vec3(0.9f, 0.3f, 0.2f), 0.4f, 0.1f, glm::vec3(0.0f), 0.0f };
            scene.transformComponents[cube.get_id()] = TransformComponent{};
            playerEntityId = cube.get_id();

            // Directional sun: drives the real lighting + shadow map path.
            auto sun = scene.create_entity("Sun");
            scene.lightComponents[sun.get_id()] = LightComponent{
                glm::vec3(1.0f, 0.95f, 0.85f), 10000.0f, 1000.0f, true };
            scene.transformComponents[sun.get_id()].rotation = glm::vec3(-45.0f, 30.0f, 0.0f);

            // Spot light: cone of 25° inner / 45° outer pointing down at the
            // cube — exercises the spotLight* arrays in the LightParams UBO.
            auto spot = scene.create_entity("Spot");
            scene.lightComponents[spot.get_id()] = LightComponent{
                glm::vec3(0.2f, 0.5f, 1.0f), 4000.0f, 18.0f, true, LightType::Spot };
            scene.transformComponents[spot.get_id()].position = glm::vec3(2.5f, 3.0f, 0.0f);
            scene.transformComponents[spot.get_id()].rotation = glm::vec3(-90.0f, 0.0f, 0.0f);

            // Area light: a 4x2 rectangle emitter facing the cube.
            auto area = scene.create_entity("Area");
            scene.lightComponents[area.get_id()] = LightComponent{
                glm::vec3(1.0f, 0.4f, 0.9f), 1500.0f, 20.0f, true, LightType::Area };
            scene.transformComponents[area.get_id()].position = glm::vec3(-3.0f, 2.5f, 1.0f);
            scene.transformComponents[area.get_id()].rotation = glm::vec3(0.0f, 90.0f, 0.0f);

            auto rb = Physics::BodyDesc{};
            rb.motion = Physics::MotionType::Dynamic;
            rb.position = glm::vec3(0, 10, 0);
            physics.create_body(rb);
        }
        // Player entity for the script-driven controller: an authored "Player"
        // entity in a scene file, or the built-in cube in the fallback scene.
        for (const auto& [id, entity] : scene.get_entities()) {
            if (entity.get_name() == "Player") { playerEntityId = id; break; }
        }
        setupGameplay();
        // Camera starts at the authored camera entity (if any).
        for (const auto& [id, cam] : scene.cameraComponents) {
            (void)cam;
            const auto tit = scene.transformComponents.find(id);
            if (tit != scene.transformComponents.end()) {
                camPos = tit->second.position;
                camYaw = tit->second.rotation.y;
                camPitch = tit->second.rotation.x;
            }
            break;
        }
        // Mount the cooked content package so mesh/material assets resolve by
        // UUID. The packaged game runs from the package root (AssetManifest.txt
        // lives there); the dev fallback location is Content/.
        std::string error;
        if (content.mount(".", &error) || content.mount("Content", &error)) {
            std::cout << "[Game] Mounted Content package (" << content.assets().size() << " asset(s))\n";
        } else {
            std::cout << "[Game] No content package found (running without cooked assets)\n";
        }
    }

void VulkanGame::spawnTargets(){
        Physics::BodyDesc floor;
        floor.motion = Physics::MotionType::Static;
        floor.position = glm::vec3(0.0f, -0.6f, 0.0f);
        floor.collider.shape = Physics::BoxShape{ glm::vec3(40.0f, 0.6f, 40.0f) };
        floor.collider.friction = 0.8f;
        physics.create_body(floor);

        auto ground = scene.create_entity("Floor");
        scene.meshRendererComponents[ground.get_id()] = MeshRendererComponent{};
        scene.materialComponents[ground.get_id()] = MaterialComponent{
            glm::vec3(0.42f, 0.42f, 0.46f), 0.3f, 0.1f, glm::vec3(0.0f), 0.0f };
        TransformComponent groundTransform;
        groundTransform.position = glm::vec3(0.0f, -0.6f, 0.0f);
        groundTransform.scale = glm::vec3(80.0f, 1.2f, 80.0f);
        scene.transformComponents[ground.get_id()] = groundTransform;

        const glm::vec3 crateColors[] = {
            { 0.9f, 0.30f, 0.22f }, { 0.25f, 0.72f, 0.32f }, { 0.22f, 0.52f, 0.92f },
            { 0.95f, 0.80f, 0.20f }, { 0.80f, 0.30f, 0.80f }, { 0.20f, 0.85f, 0.85f } };
        for (int i = 0; i < 6; ++i) {
            const int row = i / 3;
            const int col = i % 3;
            const glm::vec3 pos{ 8.0f + col * 3.0f, 0.5f + row * 1.8f,
                                 (col == 1 ? 0.0f : (row == 0 ? -1.2f : 1.2f)) };
            auto entity = scene.create_entity("Target " + std::to_string(i + 1));
            const UUID id = entity.get_id();
            scene.meshRendererComponents[id] = MeshRendererComponent{};
            scene.materialComponents[id] = MaterialComponent{
                crateColors[i], 0.55f, 0.15f, glm::vec3(0.0f), 0.0f };
            TransformComponent t;
            t.position = pos;
            scene.transformComponents[id] = t;

            Physics::BodyDesc body;
            body.motion = Physics::MotionType::Dynamic;
            body.position = pos;
            body.collider.shape = Physics::BoxShape{ glm::vec3(0.5f) };
            body.collider.friction = 0.6f;
            body.collider.restitution = 0.35f;
            body.userData = static_cast<std::uint64_t>(i + 1);
            const Physics::BodyHandle handle = physics.create_body(body);
            fpsTargets.push_back(FpsTarget{ id, handle, 0, true });
        }
        std::cout << "[Game] FPS mode: " << fpsTargets.size()
                  << " target crates on a physics floor (3 hits to destroy)\n";
    }

