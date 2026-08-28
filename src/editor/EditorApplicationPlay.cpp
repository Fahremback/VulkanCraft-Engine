#include "EditorApplication.hpp"

namespace Engine {

// the exact height function of the visual sheet. Bodies land on what they see.
void EditorApplication::build_play_world_collision() {
    m_playStaticBodies.clear();
    float minBottom = 0.0f;
    bool any = false;
    size_t voxelBoxes = 0;

    // ---- Voxel volumes: exact merged-cell boxes ----------------------------
    if (m_editorScene) {
        for (const auto& [entityId, gridPtr] : m_voxelStructures) {
            if (!gridPtr) continue;
            glm::vec3 origin(0.0f);
            const auto trIt = m_editorScene->transformComponents.find(entityId);
            if (trIt != m_editorScene->transformComponents.end())
                origin = trIt->second.position;
            const Engine::Voxel::VoxelStructure& grid = *gridPtr;
            const auto solid = [&grid](int x, int y, int z) -> bool {
                return !grid.get(Engine::Voxel::Int3{ x, y, z }).empty();
            };
            const auto boxes = Engine::Physics::merge_solid_voxels(
                kVoxelSizeX, kVoxelSizeY, kVoxelSizeZ, solid);
            for (const auto& b : boxes) {
                const glm::vec3 half(static_cast<float>(b.sx) * 0.5f,
                                     static_cast<float>(b.sy) * 0.5f,
                                     static_cast<float>(b.sz) * 0.5f);
                Physics::BodyDesc desc;
                desc.motion = Physics::MotionType::Static;
                // Same cell -> world mapping as rebuild_voxel_mesh(): grid
                // centered on the volume origin in X/Z, base at origin.y.
                desc.position = origin + glm::vec3(
                    static_cast<float>(b.x - kVoxelSizeX / 2) + half.x,
                    static_cast<float>(b.y) + half.y,
                    static_cast<float>(b.z - kVoxelSizeZ / 2) + half.z);
                desc.collider.shape = Physics::BoxShape{ half };
                desc.collider.friction = 0.7f;
                desc.collider.restitution = 0.05f;
                const Physics::BodyHandle handle = m_playPhysics.create_body(desc);
                if (handle == Physics::InvalidBody) continue;
                m_playStaticBodies.push_back(handle);
                ++voxelBoxes;
                const float bottom = desc.position.y - half.y;
                minBottom = any ? std::min(minBottom, bottom) : bottom;
                any = true;
            }
        }
    }

    // ---- Procedural terrain: sampled column boxes ---------------------------
    // 64x64 columns over the sheet. Each column runs from a common base below
    // the lowest sample up to its own height, so adjacent columns always share
    // faces — no gaps to fall through between samples.
    size_t terrainColumns = 0;
    if (m_terrainValid && m_terrainParams.segments > 0 && m_terrainParams.halfExtent > 0.0f) {
        constexpr int kCols = 64;
        constexpr size_t kMaxTerrainBodies = 4096; // hard cap for play startup
        const float half = m_terrainParams.halfExtent;
        const float cell = (2.0f * half) / static_cast<float>(kCols);
        float heights[kCols * kCols];
        float minH = 0.0f;
        for (int j = 0; j < kCols; ++j) {
            for (int i = 0; i < kCols; ++i) {
                const float x = -half + (static_cast<float>(i) + 0.5f) * cell;
                const float z = -half + (static_cast<float>(j) + 0.5f) * cell;
                const float h = terrain_surface_height(
                    m_terrainParams.seed, m_terrainParams.scale,
                    m_terrainParams.octaves, m_terrainParams.amount,
                    m_terrainParams.falloff, half, x, z);
                heights[j * kCols + i] = h;
                minH = (i == 0 && j == 0) ? h : std::min(minH, h);
            }
        }
        const float base = std::floor(minH) - 2.0f;
        for (int j = 0; j < kCols && m_playStaticBodies.size() < kMaxTerrainBodies + voxelBoxes; ++j) {
            for (int i = 0; i < kCols && m_playStaticBodies.size() < kMaxTerrainBodies + voxelBoxes; ++i) {
                const float top = heights[j * kCols + i];
                const float centerY = (top + base) * 0.5f;
                const float halfY = std::max((top - base) * 0.5f, 0.25f);
                Physics::BodyDesc desc;
                desc.motion = Physics::MotionType::Static;
                desc.position = glm::vec3(
                    -half + (static_cast<float>(i) + 0.5f) * cell, centerY,
                    -half + (static_cast<float>(j) + 0.5f) * cell);
                desc.collider.shape = Physics::BoxShape{
                    glm::vec3(cell * 0.5f, halfY, cell * 0.5f) };
                desc.collider.friction = 0.7f;
                desc.collider.restitution = 0.05f;
                const Physics::BodyHandle handle = m_playPhysics.create_body(desc);
                if (handle == Physics::InvalidBody) continue;
                m_playStaticBodies.push_back(handle);
                ++terrainColumns;
                minBottom = any ? std::min(minBottom, base) : base;
                any = true;
            }
        }
    }

    std::cout << "[PlayRuntime] world collision: " << voxelBoxes << " voxel boxes, "
              << terrainColumns << " terrain columns" << std::endl;

    // Void-failsafe plane goes BELOW everything real (or stays at y=0 when the
    // scene has no collidable content at all).
    m_playCollisionFloorY = any ? (minBottom - 8.0f) : -0.5f;
}

void EditorApplication::setup_play_runtime() {
    teardown_play_runtime();
    Scene* playScene = m_playMode.get_active_scene();
    if (!playScene) return;

    // Real world collision first (see build_play_world_collision). The wide
    // thin plane below is now only a void-failsafe placed under the lowest
    // real collider (or at y=0 when the scene has no collidable content).
    build_play_world_collision();
    {
        Physics::BodyDesc ground;
        ground.motion = Physics::MotionType::Static;
        ground.position = glm::vec3(0.0f, m_playCollisionFloorY, 0.0f);
        ground.collider.shape = Physics::BoxShape{ glm::vec3(2000.0f, 0.5f, 2000.0f) };
        ground.collider.friction = 0.7f;
        ground.collider.restitution = 0.05f;
        m_playGroundBody = m_playPhysics.create_body(ground);
    }
    for (const auto& [id, rb] : playScene->rigidbodyComponents) {
        Physics::BodyDesc desc;
        desc.motion = rb.isKinematic ? Physics::MotionType::Kinematic : Physics::MotionType::Dynamic;
        desc.mass = std::max(rb.mass, 0.01f);
        desc.collider.friction = rb.friction;
        desc.collider.restitution = rb.restitution;
        const auto tit = playScene->transformComponents.find(id);
        if (tit != playScene->transformComponents.end()) {
            desc.position = tit->second.position;
            desc.rotation = glm::quat(glm::radians(tit->second.rotation));
        }
        const Physics::BodyHandle handle = m_playPhysics.create_body(desc);
        if (handle != Physics::InvalidBody) m_playBodies[id] = handle;
    }

    // Wicked-port runtime (formerly capability marker): constraints run as
    // soft force-based constraints — the runtime solver exposes no rigid-joint
    // API (PhysicsWorld's joints are a separate, unintegrated world). Each
    // constraint stores the world anchors captured at play start; the tick
    // applies spring forces to keep the anchors together (see
    // tick_play_runtime). Springs just record a rest anchor.
    for (const auto& [id, cn] : playScene->constraintComponents) {
        if (!cn.enabled) continue;
        if (!m_playBodies.contains(id)) continue;
        const auto tit = playScene->transformComponents.find(id);
        const glm::vec3 baseA = (tit != playScene->transformComponents.end())
                                    ? tit->second.position
                                    : glm::vec3(0.0f);
        ConstraintRest rest;
        rest.anchorA = baseA + cn.anchor;
        rest.anchorB = baseA + cn.anchor; // refined when the other body exists
        if (const auto bodyBIt = m_playBodies.find(cn.otherEntity); bodyBIt != m_playBodies.end()) {
            if (Physics::RigidBody* bodyB = m_playPhysics.body(bodyBIt->second)) {
                rest.anchorB = bodyB->position + cn.anchor;
            }
        }
        rest.restLength = glm::length(rest.anchorB - rest.anchorA);
        m_constraintRests[id] = rest;
    }
    for (const auto& [id, sp] : playScene->springComponents) {
        if (sp.disabled || !sp.enabled) continue;
        if (!m_playBodies.contains(id)) continue;
        const auto tit = playScene->transformComponents.find(id);
        m_springRests[id] = (tit != playScene->transformComponents.end())
                                ? tit->second.position
                                : glm::vec3(0.0f);
    }

    // Play particles (Fase 8): one ParticleSimulation emitter per
    // ParticleEmitterComponent entity, positioned at the world transform.
    for (const auto& [id, pe] : playScene->particleEmitterComponents) {
        if (!pe.emitting) continue;
        Engine::Gameplay::ParticleEmitterDesc desc;
        desc.direction = glm::normalize(pe.direction);
        desc.coneAngle = pe.coneAngle;
        desc.rate = pe.rate;
        desc.speedMin = pe.speedMin;
        desc.speedMax = pe.speedMax;
        desc.lifetimeMin = pe.lifetimeMin;
        desc.lifetimeMax = pe.lifetimeMax;
        desc.sizeStart = pe.sizeStart;
        desc.sizeEnd = pe.sizeEnd;
        desc.colorStart = pe.colorStart;
        desc.colorEnd = pe.colorEnd;
        desc.acceleration = pe.acceleration;
        desc.drag = pe.drag;
        desc.turbulence = pe.turbulence;
        desc.restitution = pe.restitution;
        desc.collide = pe.collide;
        desc.emitting = pe.emitting;
        const auto tit = playScene->transformComponents.find(id);
        desc.position = (tit != playScene->transformComponents.end())
                            ? tit->second.position + pe.position
                            : pe.position;
        m_playEmitters[id] = m_playParticles.add_emitter(desc);
        if (pe.burstCount > 0) m_playParticles.emit_burst(m_playEmitters[id], pe.burstCount);
    }

    // Play vehicles (Fase 8): chassis body + four wheels derived from the
    // component's wheelBase/trackWidth, driven by the arrow keys in play.
    for (const auto& [id, veh] : playScene->vehicleComponents) {
        if (!veh.enabled) continue;
        Physics::BodyDesc chassis;
        chassis.motion = Physics::MotionType::Dynamic;
        chassis.mass = std::max(veh.mass, 1.0f);
        chassis.collider.shape = Physics::BoxShape{{veh.wheelBase * 0.35f, 0.35f, veh.trackWidth * 0.35f}};
        const auto tit = playScene->transformComponents.find(id);
        if (tit != playScene->transformComponents.end()) {
            chassis.position = tit->second.position;
            chassis.rotation = glm::quat(glm::radians(tit->second.rotation));
        }
        const Physics::BodyHandle body = m_playPhysics.create_body(chassis);
        if (body == Physics::InvalidBody) continue;
        m_playVehicleChassis[id] = body;
        const float halfBase = veh.wheelBase * 0.5f;
        const float halfTrack = veh.trackWidth * 0.5f;
        const glm::vec3 locals[4] = {
            {-halfBase, -0.1f, -halfTrack}, {-halfBase, -0.1f, halfTrack},
            {halfBase, -0.1f, -halfTrack},  {halfBase, -0.1f, halfTrack},
        };
        std::vector<Engine::Gameplay::WheelDesc> wheels(4);
        for (int i = 0; i < 4; ++i) {
            wheels[i].localPosition = locals[i];
            wheels[i].radius = veh.wheelRadius;
            wheels[i].suspensionRestLength = veh.suspensionRest;
            wheels[i].maxDriveForce = veh.enginePower;
            wheels[i].maxBrakeForce = veh.brakeForce;
            wheels[i].maxSteerAngle = veh.maxSteerAngle;
            wheels[i].steering = i < 2;
            wheels[i].driven = veh.frontWheelDrive ? i < 2 : i >= 2;
        }
        m_playVehicles.emplace(id, Engine::Gameplay::VehicleRuntime(body, std::move(wheels)));
    }

    // Play ragdolls (Fase 6): physics bodies per bone. With fromSkeleton set,
    // the bones come from the entity's skin skeleton (a sibling Skeleton asset
    // matching the mesh stem); otherwise a two-bone fallback is used. The play
    // physics simulates them each frame; the pose drives skinned rendering.
    for (const auto& [id, rg] : playScene->ragdollComponents) {
        if (!rg.enabled) continue;
        glm::vec3 rootPos{0.0f};
        const auto tit = playScene->transformComponents.find(id);
        if (tit != playScene->transformComponents.end()) rootPos = tit->second.position;
        rootPos += rg.spawnOffset;
        std::vector<Physics::RagdollBoneDesc> bones;
        bool fromSkin = false;
        if (rg.fromSkeleton) {
            std::string meshStem;
            if (const auto mit = playScene->meshRendererComponents.find(id); mit != playScene->meshRendererComponents.end()) {
                for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
                    if (asset.type == AssetType::Mesh && asset.id == mit->second.meshAssetID) {
                        meshStem = asset.sourcePath.stem().string();
                        break;
                    }
                }
            }
            if (!meshStem.empty()) {
                for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
                    if (asset.type != AssetType::Skeleton || asset.sourcePath.stem().string() != meshStem) continue;
                    SkeletonAsset skeleton;
                    if (AnimationAssetIO::load_skeleton(skeleton, asset.cookedPath)) {
                        bones = Physics::build_ragdoll_bones(skeleton, rg.massPerBone);
                        fromSkin = !bones.empty();
                    }
                    break;
                }
            }
            if (!fromSkin) {
                std::cout << "[Editor] Ragdoll entity " << id.to_string() << ": no sibling skeleton for mesh '"
                          << meshStem << "' — using two-bone fallback\n";
            }
        }
        if (bones.empty()) {
            bones.push_back(Physics::RagdollBoneDesc{"Root", "", glm::vec3(0.0f), glm::quat(1, 0, 0, 0), 0.6f, 0.12f, rg.massPerBone, glm::vec3(0.0f)});
            bones.push_back(Physics::RagdollBoneDesc{"Tip", "Root", glm::vec3(1.0f, 0.0f, 0.0f), glm::quat(1, 0, 0, 0), 0.6f, 0.12f, rg.massPerBone, glm::vec3(0.0f)});
        }
        Physics::Ragdoll ragdoll;
        if (ragdoll.create(m_playPhysics, bones, rootPos)) {
            m_playRagdolls.emplace(id, std::move(ragdoll));
            std::cout << "[Editor] Play ragdoll active (entity=" << id.to_string() << ", bones="
                      << bones.size() << (fromSkin ? ", from skin skeleton)\n" : ", fallback)\n");
        }
    }

    // Play missions (Fase 8): Start -> SetObjective -> WaitForEvent -> Complete.
    // The completeEvent is dispatched to the mission system by the play tick
    // when another component raises it (e.g. a weapon kill or script emit).
    for (const auto& [id, mc] : playScene->missionComponents) {
        std::vector<Engine::Gameplay::MissionNode> nodes;
        nodes.push_back(Engine::Gameplay::start_node("start", "obj"));
        nodes.push_back(Engine::Gameplay::set_objective_node("obj", "objective", mc.objectiveText, mc.objectiveTarget, "wait"));
        nodes.push_back(Engine::Gameplay::wait_for_event_node("wait", mc.completeEvent, 1, "done"));
        nodes.push_back(Engine::Gameplay::complete_mission_node("done"));
        Engine::Gameplay::Mission mission("mission_" + id.to_string(), mc.missionId, std::move(nodes));
        m_playMissions.register_mission(std::move(mission));
        m_playMissionIds[id] = mc.missionId;
        if (mc.autoStart) m_playMissions.start("mission_" + id.to_string());
    }

    // Play dialogues (Fase 8): a one-node graph with a single choice that can
    // chain to another dialogue; played on start when playOnStart is set.
    for (const auto& [id, dc] : playScene->dialogueComponents) {
        Engine::Gameplay::DialogueGraph graph;
        graph.id = dc.dialogueId;
        Engine::Gameplay::DialogueNode node;
        node.id = "line";
        node.line.character = dc.character;
        node.line.text = dc.line;
        if (!dc.choiceText.empty()) {
            Engine::Gameplay::DialogueChoice choice;
            choice.text = dc.choiceText;
            choice.nextNode = dc.nextDialogueId;   // empty = end
            node.choices.push_back(std::move(choice));
        }
        graph.nodes.push_back(std::move(node));
        m_playDialogues.register_graph(std::move(graph));
        m_playDialogueIds[id] = dc.dialogueId;
        if (dc.playOnStart) m_playDialogues.play(dc.dialogueId);
    }

    // Play audio (Fase 8): resolve the .ogg through the asset registry, decode
    // it into an AudioClip and start a voice on the mixer (spatial vs the
    // camera listener). Voices advance when the mixer is rendered each tick.
    for (const auto& [id, ac] : playScene->audioComponents) {
        if (!ac.playOnStart || ac.clipPath.empty()) continue;
        std::filesystem::path clipSource;
        std::error_code clipEc;
        const std::filesystem::path clipCanonical = std::filesystem::weakly_canonical(ac.clipPath, clipEc);
        const std::filesystem::path clipName = std::filesystem::path(ac.clipPath).filename();
        for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
            if (asset.type != AssetType::Audio) continue;
            // Match the authored clip path robustly: exact string, canonical
            // equivalence, or the same file name (the user may have typed a
            // relative path or a different separator style).
            if (asset.sourcePath == ac.clipPath || asset.sourcePath == clipCanonical ||
                asset.sourcePath.filename() == clipName) {
                clipSource = asset.sourcePath;
                break;
            }
        }
        if (clipSource.empty()) {
            std::cout << "[Editor] Play audio: no registered asset for '" << ac.clipPath << "'\n";
            continue;
        }
        const auto decoded = Engine::Audio::OggDecoder::decode_file(clipSource);
        if (!decoded || !decoded->valid()) {
            std::cout << "[Editor] Play audio: failed to decode '" << clipSource.string() << "'\n";
            continue;
        }
        auto clip = std::make_shared<Engine::Audio::AudioClip>(clipSource.filename().string());
        Engine::Audio::AudioBuffer buffer;
        buffer.sampleRate = decoded->sampleRate;
        buffer.channels = decoded->channels;
        buffer.samples = decoded->samples;
        clip->hot_swap(std::move(buffer));
        Engine::Audio::VoiceDescription desc;
        desc.clip = std::move(clip);
        desc.bus = m_playAudio.master_bus();
        desc.gain = ac.volume;
        desc.pitch = ac.pitch;
        desc.looping = ac.looping;
        desc.spatial = ac.spatial;
        const auto tit = playScene->transformComponents.find(id);
        desc.position = (tit != playScene->transformComponents.end()) ? tit->second.position : glm::vec3(0.0f);
        const Engine::Audio::VoiceId voice = m_playAudio.play(std::move(desc));
        m_playVoices[id] = voice;
        std::cout << "[Editor] Play audio voice started ('" << clipSource.filename().string() << "')\n";
    }

    // Play destructibles (Fase 8): chunkCount boxes laid out in a square grid
    // around the entity transform; weapon hits apply radial damage.
    for (const auto& [id, dc] : playScene->destructionComponents) {
        if (!dc.enabled) continue;
        const glm::vec3 center = [&]() {
            const auto tit = playScene->transformComponents.find(id);
            return (tit != playScene->transformComponents.end()) ? tit->second.position : glm::vec3(0.0f);
        }();
        const glm::quat rotation = [&]() {
            const auto tit = playScene->transformComponents.find(id);
            return (tit != playScene->transformComponents.end())
                       ? glm::quat(glm::radians(tit->second.rotation))
                       : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        }();
        const uint32_t n = std::max(dc.chunkCount, 1u);
        const uint32_t cols = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(n))));
        std::vector<Engine::Gameplay::DestructionChunkDesc> chunks;
        chunks.reserve(n);
        const glm::vec3 half = dc.chunkSize * 0.5f;
        for (uint32_t i = 0; i < n; ++i) {
            const int cx = static_cast<int>(i % cols);
            const int cy = static_cast<int>(i / cols);
            const glm::vec3 local = glm::vec3((static_cast<float>(cx) - (cols - 1) * 0.5f) * dc.chunkSize.x,
                                              (static_cast<float>(cy) - (cols - 1) * 0.5f) * dc.chunkSize.y, 0.0f);
            Engine::Gameplay::DestructionChunkDesc chunk;
            chunk.localPosition = local;
            chunk.halfExtents = half;
            chunk.mass = 1.0f;
            chunk.health = dc.chunkHealth;
            chunks.push_back(chunk);
        }
        Engine::Gameplay::DestructibleRuntime runtime;
        if (runtime.create(m_playPhysics, center, rotation, chunks)) {
            m_playDestructibles.emplace(id, std::move(runtime));
        }
    }

    // Play navigation (Fase 8): bake the public navmesh — one column per grid
    // cell of the NavigationComponent; cells covered by a play physics body
    // are omitted (blocked), exactly the footprint the legacy grid used. The
    // promoted INavigationProvider (Recast + Detour) is the navigation
    // authority (FALTANTES item 12: the grid track was removed).
    for (const auto& [id, nc] : playScene->navigationComponents) {
        if (!nc.enabled) continue;
        const auto tit = playScene->transformComponents.find(id);
        const glm::vec3 start = (tit != playScene->transformComponents.end()) ? tit->second.position : glm::vec3(0.0f);
        if (!m_playNav) m_playNav = engine::navigation::create_recast_navigation_provider();

        engine::navigation::NavmeshConfig config;
        config.boundsMinX = start.x - nc.gridWidth * nc.cellSize * 0.5f;
        config.boundsMaxX = start.x + nc.gridWidth * nc.cellSize * 0.5f;
        config.boundsMinZ = start.z - nc.gridHeight * nc.cellSize * 0.5f;
        config.boundsMaxZ = start.z + nc.gridHeight * nc.cellSize * 0.5f;
        config.boundsMinY = start.y - 8.0f;
        config.boundsMaxY = start.y + 200.0f;
        config.cellSize = nc.cellSize;
        config.cellHeight = 0.2f;
        config.agentRadius = 0.4f;
        config.agentHeight = 1.8f;
        config.agentMaxClimb = 1.0f;
        config.agentMaxSlope = 45.0f;

        std::vector<engine::navigation::VoxelColumn> columns;
        const float floorY = start.y;
        for (int gx = 0; gx < nc.gridWidth; ++gx) {
            for (int gz = 0; gz < nc.gridHeight; ++gz) {
                const float cx = config.boundsMinX + (gx + 0.5f) * nc.cellSize;
                const float cz = config.boundsMinZ + (gz + 0.5f) * nc.cellSize;
                bool blocked = false;
                for (const auto& [bid, handle] : m_playBodies) {
                    (void)bid;
                    Physics::RigidBody* body = m_playPhysics.body(handle);
                    if (!body) continue;
                    const glm::vec3 half = std::visit([](const auto& s) -> glm::vec3 {
                        using T = std::decay_t<decltype(s)>;
                        if constexpr (std::is_same_v<T, Physics::BoxShape>) return s.halfExtents;
                        else if constexpr (std::is_same_v<T, Physics::SphereShape>) return glm::vec3(s.radius);
                        else return glm::vec3(s.radius, s.halfHeight, s.radius);
                    }, body->collider.shape);
                    const glm::vec3 min = body->position - half;
                    const glm::vec3 max = body->position + half;
                    if (cx >= min.x && cx <= max.x && cz >= min.z && cz <= max.z) {
                        blocked = true;
                        break;
                    }
                }
                if (blocked) continue;
                columns.push_back({ cx, cz, floorY, floorY + 1.0f, true });
            }
        }
        if (!columns.empty()) {
            std::string navError;
            m_playNav->build(config, columns, navError);
        }
        PlayNavAgent agent;
        agent.position = start;
        agent.speed = nc.agentSpeed;
        m_playNavAgents.emplace(id, agent);
    }

    // Play-mode script: watch the scene's companion .script and compile it into
    // the play VM. OnStart starts immediately; a "Tick" event runs each frame
    // (same convention as the packaged game's player controller).
    m_playScriptPath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Content" / "Scenes" / "Initial.script";
    if (m_playScriptReloader.watch(m_playScriptPath)) {
        ScriptGraphAsset graph;
        if (graph.load(m_playScriptPath)) {
            const auto compiled = ScriptCompiler::compile(graph);
            if (compiled) {
                m_playScript.load(std::move(compiled.program));
                m_playScript.start_event("OnStart");
                m_playScriptLoaded = true;
                m_scriptDebugGraph = graph;
                m_scriptDebugger.attach(m_playScript);
                m_scriptDebuggerAttached = true;
                m_scriptPauseRequested = false;
                std::cout << "[Editor] Play script loaded: " << m_playScriptPath.string() << std::endl;
            }
        }
    }
}

// Play-world animation runtime (Animation/Timeline/IK/Retarget editors now
// Apply to the scene): the timeline animates transforms from Property tracks,
// the state machine samples clips into bone-entity transforms (bone order =
// entity hierarchy order), IK bends a two-bone chain to a target entity, and
// retargeting copies mapped bone transforms between skeletons. Animated
// entities are treated as kinematic: their play bodies follow the transforms.
void EditorApplication::tick_animation_runtime(Scene* playScene, float deltaTime) {
    if (!playScene) return;
    const auto syncBody = [&](UUID entityId) {
        const auto bodyIt = m_playBodies.find(entityId);
        if (bodyIt == m_playBodies.end()) return;
        Physics::RigidBody* body = m_playPhysics.body(bodyIt->second);
        const auto tit = playScene->transformComponents.find(entityId);
        if (!body || tit == playScene->transformComponents.end()) return;
        body->position = tit->second.position;
        body->rotation = glm::quat(glm::radians(tit->second.rotation));
        body->linearVelocity = glm::vec3(0.0f);
        body->angularVelocity = glm::vec3(0.0f);
    };
    const auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };

    // ------------------------------------------------------------------
    // Timeline: Property tracks (type 4) animate a transform. Track names
    // are "Position"/"Rotation"/"Scale" (case-insensitive) and may be
    // prefixed with an entity name ("Cube.Position") to target another
    // entity. Key values are "x y z" (position), "rx ry rz" (rotation
    // degrees) or "sx sy sz" (scale), linearly interpolated.
    // ------------------------------------------------------------------
    for (auto& [id, tl] : playScene->timelineComponents) {
        if (tl.duration <= 0.0f || tl.tracks.empty()) continue;
        tl.playhead += deltaTime;
        if (tl.playhead >= tl.duration) {
            tl.playhead = tl.loop ? std::fmod(tl.playhead, tl.duration) : tl.duration;
        }
        const float t = tl.playhead;
        for (const auto& tr : tl.tracks) {
            if (tr.muted || tr.type != 4 || tr.keys.size() < 2) continue;
            size_t i = 0;
            while (i + 1 < tr.keys.size() && tr.keys[i + 1].time <= t) ++i;
            const auto& k0 = tr.keys[i];
            const auto& k1 = (i + 1 < tr.keys.size()) ? tr.keys[i + 1] : tr.keys[i];
            float f = 0.0f;
            if (k1.time > k0.time) f = glm::clamp((t - k0.time) / (k1.time - k0.time), 0.0f, 1.0f);
            glm::vec3 v0(0.0f), v1(0.0f);
            if (std::sscanf(k0.value.c_str(), "%f %f %f", &v0.x, &v0.y, &v0.z) != 3) continue;
            if (std::sscanf(k1.value.c_str(), "%f %f %f", &v1.x, &v1.y, &v1.z) != 3) continue;
            const glm::vec3 value = glm::mix(v0, v1, f);
            UUID targetId = id;
            std::string prop = tr.name;
            const size_t dot = tr.name.find('.');
            if (dot != std::string::npos) {
                const std::string entName = tr.name.substr(0, dot);
                prop = tr.name.substr(dot + 1);
                for (const auto& [eid, ent] : playScene->get_entities()) {
                    if (ent.get_name() == entName) { targetId = eid; break; }
                }
            }
            auto target = playScene->transformComponents.find(targetId);
            if (target == playScene->transformComponents.end()) continue;
            const std::string lower = toLower(prop);
            if (lower.find("pos") != std::string::npos) target->second.position = value;
            else if (lower.find("rot") != std::string::npos) target->second.rotation = value;
            else if (lower.find("scl") != std::string::npos || lower.find("scale") != std::string::npos) target->second.scale = value;
            else continue;
            syncBody(targetId);
        }
    }

    // ------------------------------------------------------------------
    // IK: two-bone chain root -> mid -> end reaches the target entity
    // (weight-blended). If midEntity is invalid it is derived from the
    // hierarchy (the child of root that is an ancestor of end). The root
    // entity orients toward the target so the bend is visible.
    // ------------------------------------------------------------------
    for (const auto& [id, ik] : playScene->ikComponents) {
        if (!ik.enabled) continue;
        const auto rootIt = playScene->transformComponents.find(ik.rootEntity);
        const auto endIt = playScene->transformComponents.find(ik.endEntity);
        const auto tgtIt = playScene->transformComponents.find(ik.targetEntity);
        if (rootIt == playScene->transformComponents.end() ||
            endIt == playScene->transformComponents.end() ||
            tgtIt == playScene->transformComponents.end()) {
            continue;
        }
        UUID midId = ik.midEntity;
        if (!midId.is_valid()) {
            for (const auto& [eid, hc] : playScene->hierarchyComponents) {
                if (hc.parentID != ik.rootEntity) continue;
                UUID cur = ik.endEntity;
                while (cur.is_valid()) {
                    if (cur == eid) { midId = eid; break; }
                    const auto hIt = playScene->hierarchyComponents.find(cur);
                    if (hIt == playScene->hierarchyComponents.end()) break;
                    cur = hIt->second.parentID;
                }
                if (midId.is_valid()) break;
            }
        }
        const auto midIt = playScene->transformComponents.find(midId);
        if (midIt == playScene->transformComponents.end()) continue;
        const glm::vec3 a = rootIt->second.position;
        const glm::vec3 b = midIt->second.position;
        const glm::vec3 c = endIt->second.position;
        const glm::vec3 target = tgtIt->second.position;
        const float l1 = glm::length(b - a);
        const float l2 = glm::length(c - b);
        if (l1 < 1e-5f || l2 < 1e-5f) continue;
        glm::vec3 toTarget = target - a;
        const float dist = glm::length(toTarget);
        const float maxReach = l1 + l2;
        const glm::vec3 desiredEnd = (dist > maxReach && dist > 1e-5f)
                                         ? a + toTarget / dist * maxReach
                                         : target;
        // Bend axis: perpendicular to the reach direction and the pole.
        glm::vec3 reachDir = (dist > 1e-5f) ? toTarget / dist : glm::vec3(0.0f, 0.0f, 1.0f);
        glm::vec3 bend = glm::cross(reachDir, ik.poleVector);
        if (glm::length(bend) < 1e-5f) bend = glm::cross(reachDir, glm::vec3(0.0f, 1.0f, 0.0f));
        bend = glm::normalize(bend);
        // Angle at the root (law of cosines).
        const float cosA = glm::clamp((l1 * l1 + dist * dist - l2 * l2) / (2.0f * l1 * std::max(dist, 1e-5f)), -1.0f, 1.0f);
        const float angleA = std::acos(cosA);
        const glm::vec3 dirAB = glm::normalize(b - a);
        const glm::quat rotA = glm::angleAxis(angleA, bend);
        const glm::vec3 newB = a + rotA * dirAB * l1;
        // Angle at the mid joint.
        const float cosB = glm::clamp((l1 * l1 + l2 * l2 - dist * dist) / (2.0f * l1 * l2), -1.0f, 1.0f);
        const float angleB = std::acos(cosB);
        const glm::vec3 dirBC = glm::normalize(c - b);
        const glm::vec3 dirB = glm::normalize(desiredEnd - newB);
        glm::vec3 axisB = glm::cross(dirBC, dirB);
        if (glm::length(axisB) < 1e-5f) axisB = bend;
        const glm::quat rotB = glm::angleAxis(angleB, glm::normalize(axisB));
        const glm::vec3 newC = newB + rotB * dirBC * l2;
        const float w = glm::clamp(ik.weight, 0.0f, 1.0f);
        midIt->second.position = glm::mix(b, newB, w);
        endIt->second.position = glm::mix(c, newC, w);
        // Root orients toward the target so the bend reads clearly.
        const glm::vec3 fwd = glm::normalize(desiredEnd - a);
        rootIt->second.rotation = { glm::degrees(std::asin(glm::clamp(fwd.y, -1.0f, 1.0f))),
                                    glm::degrees(std::atan2(fwd.x, fwd.z)), 0.0f };
        syncBody(ik.rootEntity);
        syncBody(midId);
        syncBody(ik.endEntity);
    }

    // ------------------------------------------------------------------
    // Animation state machine: sample the current state's clip and write the
    // local pose onto the bone entities under the component entity (hierarchy
    // order = bone order). Transitions with an empty/"auto"/"true" condition
    // fire immediately; "name OP value" conditions read the state parameters.
    // ------------------------------------------------------------------
    const auto transitionSatisfied = [](const std::string& condition,
                                        const std::unordered_map<std::string, float>& params) {
        std::string c = condition;
        const auto trim = [](std::string& s) {
            const auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
            s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        };
        trim(c);
        if (c.empty() || c == "auto" || c == "true" || c == "1") return true;
        size_t opPos = std::string::npos;
        size_t opLen = 0;
        for (const char* op : {">=", "<=", "==", "!=", ">", "<"}) {
            const size_t p = c.find(op);
            if (p != std::string::npos) { opPos = p; opLen = std::strlen(op); break; }
        }
        if (opPos == std::string::npos) {
            const auto it = params.find(c);
            return it != params.end() && it->second != 0.0f;
        }
        std::string name = c.substr(0, opPos);
        trim(name);
        std::string rhs = c.substr(opPos + opLen);
        trim(rhs);
        const float lhs = [&] { const auto it = params.find(name); return it != params.end() ? it->second : 0.0f; }();
        const float rhsV = static_cast<float>(std::atof(rhs.c_str()));
        const std::string op = c.substr(opPos, opLen);
        if (op == ">") return lhs > rhsV;
        if (op == "<") return lhs < rhsV;
        if (op == ">=") return lhs >= rhsV;
        if (op == "<=") return lhs <= rhsV;
        if (op == "==") return std::abs(lhs - rhsV) < 1e-4f;
        return std::abs(lhs - rhsV) >= 1e-4f;
    };
    for (const auto& [id, an] : playScene->animationComponents) {
        if (!an.playing || an.states.empty()) continue;
        auto& rt = m_animStates[id];
        if (rt.currentState.empty()) {
            rt.currentState = an.entryState.empty() ? an.states.front().id : an.entryState;
        }
        const AnimationStateDef* state = nullptr;
        for (const auto& s : an.states) {
            if (s.id == rt.currentState) { state = &s; break; }
        }
        if (!state) {
            rt.currentState = an.states.front().id;
            state = &an.states.front();
        }
        for (const auto& tr : an.transitions) {
            if (tr.from != rt.currentState) continue;
            if (!transitionSatisfied(tr.condition, rt.params)) continue;
            for (const auto& s : an.states) {
                if (s.id == tr.to) { rt.currentState = s.id; rt.time = 0.0f; break; }
            }
            for (const auto& s : an.states) {
                if (s.id == rt.currentState) { state = &s; break; }
            }
            break;
        }
        if (state->clip == UUID{0, 0}) continue;
        auto clipIt = m_animClips.find(state->clip);
        if (clipIt == m_animClips.end()) {
            const auto metaOpt = m_assetRegistry.find(state->clip);
            if (!metaOpt) continue;
            AnimationClip clip;
            clip.id = state->clip;
            if (!AnimationAssetIO::load_clip(clip, metaOpt->sourcePath)) continue;
            clipIt = m_animClips.emplace(state->clip, std::move(clip)).first;
        }
        const AnimationClip& clip = clipIt->second;
        rt.time += deltaTime * std::max(state->speed, 0.01f);
        const float dur = std::max(clip.duration, 0.001f);
        rt.time = state->loop ? std::fmod(rt.time, dur) : std::min(rt.time, dur);
        // Build the runtime skeleton from the entity hierarchy (bone order =
        // hierarchy order) and write the sampled local pose onto the entities.
        std::vector<UUID> boneIds;
        SkeletonAsset skeleton;
        skeleton.id = id;
        skeleton.name = "PlayRuntime";
        std::function<void(UUID, int)> collect = [&](UUID eid, int parentIndex) {
            BoneNode bn;
            bn.parentIndex = parentIndex;
            const auto eIt = playScene->get_entities().find(eid);
            bn.name = (eIt != playScene->get_entities().end()) ? eIt->second.get_name() : "bone";
            skeleton.bones.push_back(bn);
            boneIds.push_back(eid);
            const int idx = static_cast<int>(skeleton.bones.size()) - 1;
            const auto hc = playScene->hierarchyComponents.find(eid);
            if (hc != playScene->hierarchyComponents.end()) {
                for (const auto& child : hc->second.childrenIDs) collect(child, idx);
            }
        };
        collect(id, -1);
        if (skeleton.bones.empty()) continue;
        const Pose pose = AnimationSampler::sample(skeleton, clip, rt.time);
        for (size_t i = 0; i < pose.local.size() && i < boneIds.size(); ++i) {
            const auto tIt = playScene->transformComponents.find(boneIds[i]);
            if (tIt == playScene->transformComponents.end()) continue;
            tIt->second.position = pose.local[i].translation;
            tIt->second.rotation = glm::degrees(glm::eulerAngles(pose.local[i].rotation));
            tIt->second.scale = pose.local[i].scale;
            syncBody(boneIds[i]);
        }
    }

    // ------------------------------------------------------------------
    // Retarget: copy mapped source-bone transforms onto target-bone entities
    // (translation scaled, rotation offset applied). Runs independently, so a
    // mapped "Hand.L" -> "Hand.R" pair mirrors even without an animation.
    // ------------------------------------------------------------------
    for (const auto& [id, rt] : playScene->retargetComponents) {
        for (const auto& m : rt.mapping) {
            UUID srcId{0, 0}, dstId{0, 0};
            for (const auto& [eid, ent] : playScene->get_entities()) {
                if (ent.get_name() == m.sourceBone) srcId = eid;
                else if (ent.get_name() == m.targetBone) dstId = eid;
            }
            if (!srcId.is_valid() || !dstId.is_valid()) continue;
            const auto src = playScene->transformComponents.find(srcId);
            const auto dst = playScene->transformComponents.find(dstId);
            if (src == playScene->transformComponents.end() ||
                dst == playScene->transformComponents.end()) {
                continue;
            }
            dst->second.position = src->second.position * m.translationScale;
            dst->second.rotation = src->second.rotation + m.rotationOffset;
            dst->second.scale = src->second.scale;
            syncBody(dstId);
        }
    }
}

// ===========================================================================
// Runtime-wired Wicked-port features (frontend port): hair strands, soft-body
// cloth, video flipbooks, gaussian splats and env-probe cubemap captures.
// Runs in Edit AND Play so authored features preview live in the viewport.
// ===========================================================================

void EditorApplication::tick_special_runtimes(Scene* scene, float deltaTime) {
    if (!scene || m_device == VK_NULL_HANDLE) return;
    const float dt = glm::clamp(deltaTime, 0.0f, 1.0f / 20.0f);

    // ---- Hair strands (verlet: gravity * gravityPower, drag, stiffness) ----
    for (auto& [id, h] : scene->hairParticleComponents) {
        if (!h.enabled) continue;
        const auto tit = scene->transformComponents.find(id);
        if (tit == scene->transformComponents.end()) continue;
        ensure_hair_sim(id, h, tit->second);
        auto it = m_hairs.find(id);
        if (it == m_hairs.end()) continue;
        HairSim& sim = it->second;
        const float stiffness = 0.15f + 0.8f * h.stiffness;
        const float drag = 1.0f - glm::clamp(h.drag, 0.0f, 0.92f);
        const float g = -9.81f * h.gravityPower;
        const float windAmp = 0.5f;
        const float windPhase = m_skyTime * 2.0f;
        for (size_t i = 0; i < sim.pos.size(); ++i) {
            const glm::vec3 vel = (sim.pos[i] - sim.prev[i]) * drag;
            glm::vec3 next = sim.pos[i] + vel;
            next.y += g * dt;
            next.x += windAmp * std::sin(windPhase + sim.pos[i].x * 1.3f + sim.pos[i].z * 0.7f) * dt;
            // Stiffness pulls each segment back toward its rest position.
            next += (sim.rest[i] - sim.pos[i]) * stiffness * dt;
            sim.prev[i] = sim.pos[i];
            sim.pos[i] = next;
        }
        upload_hair(sim, h);
    }

    // ---- Soft body cloth (verlet grid, top row pinned, ground collision) ----
    for (auto& [id, s] : scene->softBodyComponents) {
        if (!s.enabled) continue;
        const auto tit = scene->transformComponents.find(id);
        if (tit == scene->transformComponents.end()) continue;
        ensure_softbody_sim(id, s, tit->second);
        auto it = m_softBodies.find(id);
        if (it == m_softBodies.end()) continue;
        SoftBodySim& sim = it->second;
        const size_t side = static_cast<size_t>(s.detail) + 1;
        const float drag = 1.0f - glm::clamp(s.friction, 0.0f, 0.9f) * 0.45f;
        const float g = -9.81f * s.mass;
        const float windAmp = s.wind ? 1.1f : 0.0f;
        const float windPhase = m_skyTime * 3.0f;
        for (size_t i = 0; i < sim.pos.size(); ++i) {
            glm::vec3 vel = (sim.pos[i] - sim.prev[i]) * drag;
            vel.y += g * dt;
            if (windAmp > 0.0f) {
                vel.x += windAmp * std::sin(windPhase + sim.pos[i].z * 0.9f) * dt;
                vel.z += windAmp * 0.35f * std::cos(windPhase + sim.pos[i].x * 0.7f) * dt;
            }
            glm::vec3 next = sim.pos[i] + vel;
            sim.prev[i] = sim.pos[i];
            sim.pos[i] = next;
        }
        // Pressure inflates the cloth outward from its center (local space).
        if (s.pressure > 0.0f) {
            const glm::vec3 center(0.0f, -0.4f, 0.0f);
            for (size_t i = 0; i < sim.pos.size(); ++i) {
                const glm::vec3 dir = sim.pos[i] - center;
                sim.pos[i] += glm::normalize(dir) * (s.pressure * 0.15f * dt);
            }
        }
        // Structural distance constraints (a few iterations keep it stable).
        for (int iter = 0; iter < 3; ++iter) {
            for (size_t r = 0; r < s.detail; ++r) {
                for (size_t c = 0; c < s.detail; ++c) {
                    const size_t i = r * side + c;
                    const size_t right = i + 1;
                    const float restR = glm::length(sim.rest[right] - sim.rest[i]);
                    glm::vec3 delta = sim.pos[right] - sim.pos[i];
                    const float dist = glm::length(delta);
                    if (dist > 1e-6f && restR > 1e-6f) {
                        const glm::vec3 corr = delta / dist * (dist - restR) * 0.5f;
                        sim.pos[i] += corr;
                        sim.pos[right] -= corr;
                    }
                    const size_t down = i + side;
                    const float restD = glm::length(sim.rest[down] - sim.rest[i]);
                    delta = sim.pos[down] - sim.pos[i];
                    const float dist2 = glm::length(delta);
                    if (dist2 > 1e-6f && restD > 1e-6f) {
                        const glm::vec3 corr = delta / dist2 * (dist2 - restD) * 0.5f;
                        sim.pos[i] += corr;
                        sim.pos[down] -= corr;
                    }
                }
            }
        }
        // Pin the top row to the rest pose (a hanging curtain).
        for (size_t c = 0; c < side; ++c) {
            sim.pos[c] = sim.rest[c];
            sim.prev[c] = sim.rest[c];
        }
        // Ground collision in local space (entity origin plane).
        for (size_t i = 0; i < sim.pos.size(); ++i) {
            if (sim.pos[i].y < 0.0f) {
                sim.pos[i].y = 0.0f;
                sim.prev[i].y = 0.0f;
            }
        }
        upload_softbody(sim, s);
    }

    // ---- Video flipbooks: advance the playhead at fps ----
    for (auto& [id, v] : scene->videoComponents) {
        (void)id;
        if (!v.enabled || !v.playing || v.framePaths.empty()) continue;
        v.time += dt;
        const float frameDur = 1.0f / std::max(v.fps, 0.1f);
        if (v.time >= frameDur) {
            v.time = 0.0f;
            v.currentFrame++;
            if (v.currentFrame >= static_cast<int>(v.framePaths.size())) {
                if (v.loop) v.currentFrame = 0;
                else {
                    v.currentFrame = static_cast<int>(v.framePaths.size()) - 1;
                    v.playing = false;
                }
            }
        }
    }

    // ---- Gaussian splats: mark for rebuild when regenerate is requested ----
    for (auto& [id, gs] : scene->gaussianSplatComponents) {
        if (!gs.regenerate) continue;
        gs.regenerate = false;
        auto it = m_splatClouds.find(id);
        if (it != m_splatClouds.end()) it->second.dirty = true;
    }

    // ---- Expressions: apply facial weights as squash/stretch of the head ----
    for (const auto& [id, ex] : scene->expressionComponents) {
        (void)id;
        if (!ex.enabled || !ex.headEntity.is_valid()) continue;
        const auto hit = scene->transformComponents.find(ex.headEntity);
        if (hit == scene->transformComponents.end()) continue;
        const float sx = 1.0f + ex.smile * 0.15f - ex.frown * 0.10f + ex.surprised * 0.22f + ex.anger * 0.06f;
        const float sy = 1.0f - ex.blink * 0.35f + ex.surprised * 0.26f - ex.smile * 0.06f - ex.frown * 0.04f;
        const float sz = 1.0f + ex.frown * 0.08f - ex.blink * 0.10f + ex.anger * 0.05f + ex.surprised * 0.06f;
        hit->second.scale = ex.baseScale * glm::vec3(sx, sy, sz);
    }

    // ---- Env probes: one-shot capture request + periodic real-time ----
    m_envCaptureTimer += dt;
    if (m_envCaptureTimer >= 0.5f) m_envCaptureTimer = 0.0f;
    for (auto& [id, ep] : scene->envProbeComponents) {
        if (!ep.enabled) continue;
        const bool captureNow = ep.captureRequested || (ep.realTime && m_envCaptureTimer <= 0.0f);
        if (ep.captureRequested) ep.captureRequested = false;
        if (!captureNow) continue;
        const auto tit = scene->transformComponents.find(id);
        if (tit != scene->transformComponents.end()) capture_env_probe(id, ep, tit->second);
    }
}

void EditorApplication::ensure_hair_sim(const UUID& id, const HairParticleComponent& h,
                                        const TransformComponent& t) {
    auto it = m_hairs.find(id);
    const uint32_t expectedVerts = (h.segments + 1) * h.count;
    if (it != m_hairs.end() && it->second.built &&
        it->second.pos.size() == expectedVerts && h.seed == 0) {
        return;
    }
    HairSim sim;
    sim.pos.resize(expectedVerts);
    sim.prev.resize(expectedVerts);
    sim.rest.resize(expectedVerts);
    std::mt19937 rng(h.seed != 0 ? h.seed
                                 : 1337u + static_cast<uint32_t>(id.get_high() ^ id.get_low()));
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    // Roots: sample the mesh surface (first vertex of the mesh asset) or a
    // head-like sphere when no mesh is set.
    const glm::mat4 model = model_from_transform(t);
    const glm::vec3 up = glm::normalize(glm::mat3(model) * glm::vec3(0, 1, 0));
    const glm::vec3 tangent = glm::normalize(glm::mat3(model) * glm::vec3(1, 0, 0));
    const glm::vec3 bitangent = glm::normalize(glm::cross(tangent, up));
    for (uint32_t s = 0; s < h.count; ++s) {
        const float u = unit(rng), v = unit(rng);
        const float theta = u * glm::two_pi<float>();
        const float phi = std::acos(1.0f - 2.0f * v);
        // Head shell radius 0.24 (matches the humanoid head box half-extent).
        const glm::vec3 rootLocal(0.24f * std::sin(phi) * std::cos(theta),
                                  0.18f + 0.24f * std::cos(phi),
                                  0.24f * std::sin(phi) * std::sin(theta));
        const glm::vec3 root = glm::vec3(model * glm::vec4(rootLocal, 1.0f));
        // Strand direction: mostly up with a random tilt (randomness).
        const glm::vec3 tilt = glm::normalize(
            tangent * (unit(rng) - 0.5f) * 2.0f * h.randomness +
            bitangent * (unit(rng) - 0.5f) * 2.0f * h.randomness);
        const glm::vec3 dir = glm::normalize(up + tilt * 0.6f);
        const float segLen = h.length / static_cast<float>(h.segments);
        for (uint32_t seg = 0; seg <= h.segments; ++seg) {
            const size_t idx = static_cast<size_t>(s) * (h.segments + 1) + seg;
            sim.rest[idx] = root + dir * (segLen * static_cast<float>(seg));
            sim.pos[idx] = sim.rest[idx];
            sim.prev[idx] = sim.rest[idx];
        }
    }
    sim.built = true;
    m_hairs[id] = std::move(sim);
}

void EditorApplication::upload_hair(HairSim& sim, const HairParticleComponent& h) {
    if (sim.pos.empty()) return;
    std::vector<EditorVertex> verts;
    verts.reserve(sim.pos.size() * 2);
    for (uint32_t s = 0; s < h.count; ++s) {
        for (uint32_t seg = 0; seg < h.segments; ++seg) {
            const size_t a = static_cast<size_t>(s) * (h.segments + 1) + seg;
            const size_t b = a + 1;
            EditorVertex va, vb;
            va.pos = sim.pos[a];
            vb.pos = sim.pos[b];
            va.normal = vb.normal = glm::vec3(0, 1, 0);
            va.color = vb.color = h.color;
            verts.push_back(va);
            verts.push_back(vb);
        }
    }
    const VkDeviceSize size = sizeof(EditorVertex) * verts.size();
    if (sim.vb.buffer == VK_NULL_HANDLE || sim.vb.size < size) {
        if (sim.vb.buffer != VK_NULL_HANDLE) destroy_buffer(sim.vb);
        create_buffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      sim.vb.buffer, sim.vb.memory);
    }
    safe_map_and_copy(m_device, sim.vb.memory, 0, size, verts.data());
    sim.vertexCount = static_cast<uint32_t>(verts.size());
}

void EditorApplication::ensure_softbody_sim(const UUID& id, const SoftBodyComponent& s,
                                            const TransformComponent& t) {
    auto it = m_softBodies.find(id);
    const size_t side = static_cast<size_t>(s.detail) + 1;
    const size_t expected = side * side;
    if (it != m_softBodies.end() && it->second.built && it->second.pos.size() == expected) return;
    (void)t;
    SoftBodySim sim;
    sim.pos.resize(expected);
    sim.prev.resize(expected);
    sim.rest.resize(expected);
    const float half = 1.0f;
    for (size_t r = 0; r < side; ++r) {
        for (size_t c = 0; c < side; ++c) {
            const size_t i = r * side + c;
            const float x = (static_cast<float>(c) / s.detail - 0.5f) * 2.0f * half;
            const float z = (static_cast<float>(r) / s.detail - 0.5f) * 2.0f * half;
            sim.rest[i] = glm::vec3(x, 0.0f, z);
            sim.pos[i] = sim.rest[i];
            sim.prev[i] = sim.rest[i];
        }
    }
    sim.indices.clear();
    sim.indices.reserve(s.detail * s.detail * 6);
    for (size_t r = 0; r < s.detail; ++r) {
        for (size_t c = 0; c < s.detail; ++c) {
            const uint32_t a = static_cast<uint32_t>(r * side + c);
            const uint32_t b = static_cast<uint32_t>(a + 1);
            const uint32_t cc = static_cast<uint32_t>(a + side);
            const uint32_t d = static_cast<uint32_t>(cc + 1);
            sim.indices.push_back(a); sim.indices.push_back(b); sim.indices.push_back(d);
            sim.indices.push_back(a); sim.indices.push_back(d); sim.indices.push_back(cc);
        }
    }
    sim.indexCount = static_cast<uint32_t>(sim.indices.size());
    sim.built = true;
    m_softBodies[id] = std::move(sim);
}

void EditorApplication::upload_softbody(SoftBodySim& sim, const SoftBodyComponent& s) {
    if (sim.pos.empty()) return;
    std::vector<EditorVertex> verts;
    verts.reserve(sim.pos.size());
    const glm::vec3 color = glm::vec3(0.75f, 0.45f, 0.95f);
    for (const glm::vec3& p : sim.pos) {
        EditorVertex v;
        v.pos = p;
        v.normal = glm::vec3(0, 1, 0);
        v.color = color;
        verts.push_back(v);
    }
    const VkDeviceSize vs = sizeof(EditorVertex) * verts.size();
    const VkDeviceSize is = sizeof(uint32_t) * sim.indices.size();
    if (sim.vb.buffer == VK_NULL_HANDLE || sim.vb.size < vs) {
        if (sim.vb.buffer != VK_NULL_HANDLE) destroy_buffer(sim.vb);
        create_buffer(vs, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      sim.vb.buffer, sim.vb.memory);
    }
    if (sim.ib.buffer == VK_NULL_HANDLE || sim.ib.size < is) {
        if (sim.ib.buffer != VK_NULL_HANDLE) destroy_buffer(sim.ib);
        create_buffer(is, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      sim.ib.buffer, sim.ib.memory);
    }
    safe_map_and_copy(m_device, sim.vb.memory, 0, vs, verts.data());
    safe_map_and_copy(m_device, sim.ib.memory, 0, is, sim.indices.data());
}

void EditorApplication::generate_splat_cloud(const GaussianSplatComponent& gs,
                                             std::vector<EditorVertex>& verts) const {
    verts.clear();
    verts.reserve(gs.count);
    std::mt19937 rng(gs.seed != 0 ? gs.seed : 1u);
    std::uniform_real_distribution<float> box(-gs.scale * 0.5f, gs.scale * 0.5f);
    std::uniform_real_distribution<float> hue(0.0f, 1.0f);
    for (uint32_t i = 0; i < gs.count; ++i) {
        EditorVertex v;
        v.pos = glm::vec3(box(rng), box(rng), box(rng));
        // Pastel palette via golden-ratio hue.
        const float hh = hue(rng);
        const float s = 0.55f + 0.4f * hue(rng);
        const float l = 0.45f + 0.3f * hue(rng);
        glm::vec3 rgb;
        const float c = (1.0f - std::abs(2.0f * l - 1.0f)) * s;
        const float x = c * (1.0f - std::abs(std::fmod(hh * 6.0f, 2.0f) - 1.0f));
        const float m = l - c * 0.5f;
        if (hh < 1.0f / 6.0f) rgb = { c, x, 0 };
        else if (hh < 2.0f / 6.0f) rgb = { x, c, 0 };
        else if (hh < 3.0f / 6.0f) rgb = { 0, c, x };
        else if (hh < 4.0f / 6.0f) rgb = { 0, x, c };
        else if (hh < 5.0f / 6.0f) rgb = { x, 0, c };
        else rgb = { c, 0, x };
        v.color = rgb + m;
        v.normal = glm::vec3(0, 1, 0);
        verts.push_back(v);
    }
}

void EditorApplication::rebuild_paint_buffer(const UUID& id, PaintComponent& pc,
                                             const EditorMeshResource* mesh) {
    auto it = m_paintBuffers.find(id);
    if (it == m_paintBuffers.end() || it->second.dirty) {
        if (!mesh || mesh->cpuPositions.empty()) return;
        const size_t n = mesh->cpuPositions.size();
        std::vector<EditorVertex> verts;
        verts.reserve(n);
        const bool hasColors = pc.vertexColors.size() == n;
        for (size_t i = 0; i < n; ++i) {
            EditorVertex v;
            v.pos = mesh->cpuPositions[i];
            v.normal = glm::vec3(0, 1, 0);
            v.color = hasColors ? glm::vec3(pc.vertexColors[i]) : glm::vec3(1.0f);
            verts.push_back(v);
        }
        const VkDeviceSize vs = sizeof(EditorVertex) * verts.size();
        if (it == m_paintBuffers.end()) it = m_paintBuffers.emplace(id, PaintData{}).first;
        if (it->second.vb.buffer != VK_NULL_HANDLE && it->second.vb.size < vs) {
            destroy_buffer(it->second.vb);
            it->second.vb = GPUBuffer{};
        }
        if (it->second.vb.buffer == VK_NULL_HANDLE) {
            create_buffer(vs, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          it->second.vb.buffer, it->second.vb.memory);
        }
        safe_map_and_copy(m_device, it->second.vb.memory, 0, vs, verts.data());
        it->second.vertexCount = static_cast<uint32_t>(n);
        it->second.dirty = false;
    }
}

void EditorApplication::capture_env_probe(const UUID& id, const EnvProbeComponent& ep,
                                          const TransformComponent& t) {
    if (m_device == VK_NULL_HANDLE) return;
    const uint32_t size = ep.resolution >= 64 ? ep.resolution : 256;
    const bool recreate = !m_envCapture.valid || m_envCapture.size != size ||
                          m_envCapture.entity != id;
    // Rebuild the cubemap target when the probe/resolution changes.
    if (recreate) {
        if (m_envCapture.valid) {
            for (int i = 0; i < 6; ++i) {
                if (m_envCapture.framebuffers[i]) vkDestroyFramebuffer(m_device, m_envCapture.framebuffers[i], nullptr);
                if (m_envCapture.views[i]) vkDestroyImageView(m_device, m_envCapture.views[i], nullptr);
            }
            if (m_envCapture.image) vkDestroyImage(m_device, m_envCapture.image, nullptr);
            if (m_envCapture.memory) vkFreeMemory(m_device, m_envCapture.memory, nullptr);
            if (m_envCapture.renderPass) vkDestroyRenderPass(m_device, m_envCapture.renderPass, nullptr);
            if (m_envCapture.sampler) vkDestroySampler(m_device, m_envCapture.sampler, nullptr);
            m_envCapture = EnvProbeCapture{};
        }
        const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
        const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
        // Cubemap color image (6 layers).
        VkImageCreateInfo img{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        img.imageType = VK_IMAGE_TYPE_2D;
        img.extent = { size, size, 1 };
        img.mipLevels = 1;
        img.arrayLayers = 6;
        img.format = colorFormat;
        img.tiling = VK_IMAGE_TILING_OPTIMAL;
        img.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        img.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        img.samples = VK_SAMPLE_COUNT_1_BIT;
        img.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        img.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        if (vkCreateImage(m_device, &img, nullptr, &m_envCapture.image) != VK_SUCCESS) return;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(m_device, m_envCapture.image, &req);
        VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_device, &alloc, nullptr, &m_envCapture.memory) != VK_SUCCESS) return;
        vkBindImageMemory(m_device, m_envCapture.image, m_envCapture.memory, 0);
        // 2D-array views, one per face.
        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = m_envCapture.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = colorFormat;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        for (int i = 0; i < 6; ++i) {
            view.subresourceRange.baseArrayLayer = static_cast<uint32_t>(i);
            if (vkCreateImageView(m_device, &view, nullptr, &m_envCapture.views[i]) != VK_SUCCESS) return;
        }
        // Depth image (single 2D, shared by all faces).
        VkImage depthImage = VK_NULL_HANDLE;
        VkDeviceMemory depthMemory = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
        create_image(size, size, depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, depthImage, depthMemory);
        depthView = create_image_view(depthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
        // Render pass: color + depth.
        VkAttachmentDescription attachments[2]{};
        attachments[0].format = colorFormat;
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        attachments[1].format = depthFormat;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkAttachmentReference depthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;
        VkRenderPassCreateInfo rpInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rpInfo.attachmentCount = 2;
        rpInfo.pAttachments = attachments;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;
        if (vkCreateRenderPass(m_device, &rpInfo, nullptr, &m_envCapture.renderPass) != VK_SUCCESS) return;
        for (int i = 0; i < 6; ++i) {
            VkImageView fbViews[2] = { m_envCapture.views[i], depthView };
            VkFramebufferCreateInfo fb{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fb.renderPass = m_envCapture.renderPass;
            fb.attachmentCount = 2;
            fb.pAttachments = fbViews;
            fb.width = size;
            fb.height = size;
            fb.layers = 1;
            if (vkCreateFramebuffer(m_device, &fb, nullptr, &m_envCapture.framebuffers[i]) != VK_SUCCESS) return;
        }
        // Sampler + descriptor update.
        VkSamplerCreateInfo samp{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samp.magFilter = VK_FILTER_LINEAR;
        samp.minFilter = VK_FILTER_LINEAR;
        samp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samp.addressModeU = samp.addressModeV = samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(m_device, &samp, nullptr, &m_envCapture.sampler);
        VkImageView cubeView = VK_NULL_HANDLE;
        VkImageViewCreateInfo cv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        cv.image = m_envCapture.image;
        cv.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        cv.format = colorFormat;
        cv.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
        vkCreateImageView(m_device, &cv, nullptr, &cubeView);
        VkDescriptorImageInfo descImg{};
        descImg.sampler = m_envCapture.sampler;
        descImg.imageView = cubeView;
        descImg.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = m_envCapture.descriptorSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &descImg;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
        // Keep the cube view alive for the lifetime of the capture.
        m_envCubeView = cubeView;
        m_envDepthView = depthView;
        m_envDepthImage = depthImage;
        m_envDepthMemory = depthMemory;
        m_envCapture.size = size;
        m_envCapture.entity = id;
        m_envCapture.valid = true;
    }
    m_envCapturePending = true;
}

UUID EditorApplication::resolve_texture_asset_by_name(const std::string& name) const {
    for (const AssetMetadata& meta : m_assetRegistry.snapshot()) {
        if (meta.type == AssetType::Texture && meta.sourcePath.filename().string() == name) {
            return meta.id;
        }
    }
    return UUID{ 0, 0 };
}

void EditorApplication::record_env_face(VkCommandBuffer cmd, const glm::mat4& view, const glm::mat4& proj,
                                        const glm::vec3& pos, Scene* scene) {
    if (!scene || m_device == VK_NULL_HANDLE) return;
    (void)pos;
    const glm::mat4 viewProj = proj * view;
    // Terrain + voxel volumes (the shared scene helpers).
    if (m_terrainValid && m_terrainVB.buffer != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
        const glm::mat4 model(1.0f);
        draw_indexed_editor_mesh(cmd, m_scenePipelineLayout, m_terrainVB.buffer, m_terrainIB.buffer,
                                 m_terrainIndexCount, viewProj * model, glm::vec4(1.0f));
    }
    draw_voxel_volumes(cmd, viewProj, scene);
    // Entities with a mesh renderer (flat shading — the capture is a
    // geometry/lighting preview for the reflection probe).
    for (const auto& [id, ent] : scene->get_entities()) {
        (void)ent;
        const auto tit = scene->transformComponents.find(id);
        if (tit == scene->transformComponents.end()) continue;
        const auto layerIt = scene->layerComponents.find(id);
        if (layerIt != scene->layerComponents.end() && !layerIt->second.visible) continue;
        const auto meshIt = scene->meshRendererComponents.find(id);
        if (meshIt == scene->meshRendererComponents.end()) continue;
        glm::vec3 baseColor(0.72f, 0.75f, 0.82f);
        const auto matIt = scene->materialComponents.find(id);
        if (matIt != scene->materialComponents.end()) baseColor = matIt->second.albedo;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_scenePipeline);
        if (const auto* mesh = get_mesh_resource(meshIt->second.meshAssetID)) {
            draw_mesh_resource(cmd, viewProj * model_from_transform(tit->second),
                               glm::vec4(baseColor, 1.0f), *mesh);
        } else {
            draw_indexed_cube(cmd, m_scenePipelineLayout, m_cubeVB.buffer, m_cubeIB.buffer,
                              m_cubeIndexCount, viewProj * model_from_transform(tit->second),
                              glm::vec4(baseColor, 1.0f));
        }
    }
}

glm::vec3 EditorApplication::viewport_mouse_dir(const glm::vec2& mouseScreen) const {
    const float aspect = static_cast<float>(m_offscreen.width) / std::max(1u, m_offscreen.height);
    const glm::mat4 invViewProj =
        glm::inverse(m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix());
    const float ndcX = (mouseScreen.x - m_viewportImagePos.x) / std::max(1.0f, m_viewportImageSize.x) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (mouseScreen.y - m_viewportImagePos.y) / std::max(1.0f, m_viewportImageSize.y) * 2.0f;
    const glm::vec4 near4 = invViewProj * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    const glm::vec4 far4 = invViewProj * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    const glm::vec3 nearP = glm::vec3(near4) / near4.w;
    const glm::vec3 farP = glm::vec3(far4) / far4.w;
    return glm::normalize(farP - nearP);
}

bool EditorApplication::paint_mesh_stroke(const glm::vec3& origin, const glm::vec3& dir) {
    if (!m_editorScene) return false;
    Scene* scene = m_editorScene.get();
    const UUID target = m_selectedEntity.is_valid() ? m_selectedEntity.get_id() : UUID{ 0, 0 };
    if (!target.is_valid() || !scene->paintComponents.contains(target)) return false;
    const auto meshComp = scene->meshRendererComponents.find(target);
    if (meshComp == scene->meshRendererComponents.end() ||
        !meshComp->second.meshAssetID.is_valid()) {
        return false;
    }
    const auto* mesh = get_mesh_resource(meshComp->second.meshAssetID);
    if (!mesh || mesh->cpuPositions.empty()) return false;
    const auto tit = scene->transformComponents.find(target);
    if (tit == scene->transformComponents.end()) return false;
    const glm::mat4 model = model_from_transform(tit->second);
    const glm::mat4 invModel = glm::inverse(model);
    const glm::vec3 localOrigin = glm::vec3(invModel * glm::vec4(origin, 1.0f));
    const glm::vec3 localDir = glm::normalize(glm::vec3(invModel * glm::vec4(dir, 0.0f)));
    const std::vector<glm::vec3>& P = mesh->cpuPositions;
    const std::vector<uint32_t>& idx = mesh->cpuIndices;
    // Möller–Trumbore against the mesh triangles (local space).
    const auto rayTri = [](const glm::vec3& o, const glm::vec3& d,
                           const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                           float& t, glm::vec3& bary) -> bool {
        const glm::vec3 e1 = b - a;
        const glm::vec3 e2 = c - a;
        const glm::vec3 pv = glm::cross(d, e2);
        const float det = glm::dot(e1, pv);
        if (std::abs(det) < 1e-8f) return false;
        const float invDet = 1.0f / det;
        const glm::vec3 tv = o - a;
        const float u = glm::dot(tv, pv) * invDet;
        if (u < 0.0f || u > 1.0f) return false;
        const glm::vec3 qv = glm::cross(tv, e1);
        const float v = glm::dot(d, qv) * invDet;
        if (v < 0.0f || u + v > 1.0f) return false;
        t = glm::dot(e2, qv) * invDet;
        if (t < 0.0f) return false;
        bary = { 1.0f - u - v, u, v };
        return true;
    };
    float bestT = 1e30f;
    glm::vec3 bestHit(0.0f);
    if (idx.empty()) {
        for (size_t i = 0; i + 2 < P.size(); i += 3) {
            float t;
            glm::vec3 bary;
            if (rayTri(localOrigin, localDir, P[i], P[i + 1], P[i + 2], t, bary) && t < bestT) {
                bestT = t;
                bestHit = P[i] * bary.x + P[i + 1] * bary.y + P[i + 2] * bary.z;
            }
        }
    } else {
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            const glm::vec3& a = P[idx[i]];
            const glm::vec3& b = P[idx[i + 1]];
            const glm::vec3& c = P[idx[i + 2]];
            float t;
            glm::vec3 bary;
            if (rayTri(localOrigin, localDir, a, b, c, t, bary) && t < bestT) {
                bestT = t;
                bestHit = a * bary.x + b * bary.y + c * bary.z;
            }
        }
    }
    if (bestT > 1e29f) return false;
    PaintComponent& pc = scene->paintComponents[target];
    if (pc.vertexColors.size() != P.size()) pc.vertexColors.assign(P.size(), glm::vec4(1.0f));
    const float brush = std::max(pc.brushSize, 0.01f);
    for (size_t i = 0; i < P.size(); ++i) {
        const float d = glm::distance(P[i], bestHit);
        if (d <= brush) {
            const float falloff = 1.0f - d / brush;
            const float op = pc.opacity * falloff;
            const glm::vec3 brushCol = pc.brushColor;
            const glm::vec3 oldCol(pc.vertexColors[i]);
            pc.vertexColors[i] = glm::vec4(glm::mix(oldCol, brushCol, glm::clamp(op, 0.0f, 1.0f)), 1.0f);
        }
    }
    const auto pit = m_paintBuffers.find(target);
    if (pit != m_paintBuffers.end()) pit->second.dirty = true;
    mark_scene_dirty();
    return true;
}

void EditorApplication::record_env_capture(VkCommandBuffer cmd, Scene* scene) {
    if (!m_envCapturePending || !m_envCapture.valid || !scene) return;
    m_envCapturePending = false;
    const auto tit = scene->transformComponents.find(m_envCapture.entity);
    if (tit == scene->transformComponents.end()) return;
    const glm::vec3 pos = tit->second.position;
    const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 500.0f);
    const glm::vec3 faces[6][2] = {
        { { 1, 0, 0 }, { 0, -1, 0 } }, { { -1, 0, 0 }, { 0, -1, 0 } },
        { { 0, 1, 0 }, { 0, 0, 1 } },  { { 0, -1, 0 }, { 0, 0, -1 } },
        { { 0, 0, 1 }, { 0, -1, 0 } }, { { 0, 0, -1 }, { 0, -1, 0 } },
    };
    for (int i = 0; i < 6; ++i) {
        VkRenderPassBeginInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        info.renderPass = m_envCapture.renderPass;
        info.framebuffer = m_envCapture.framebuffers[i];
        info.renderArea.offset = { 0, 0 };
        info.renderArea.extent = { m_envCapture.size, m_envCapture.size };
        VkClearValue clears[2];
        clears[0].color = { { 0.11f, 0.13f, 0.18f, 1.0f } };
        clears[1].depthStencil = { 1.0f, 0 };
        info.clearValueCount = 2;
        info.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
        set_viewport_scissor(cmd, m_envCapture.size, m_envCapture.size);
        record_env_face(cmd, glm::lookAt(pos, pos + faces[i][0], faces[i][1]), proj, pos, scene);
        vkCmdEndRenderPass(cmd);
    }
}

void EditorApplication::tick_play_runtime(float deltaTime) {
    const PlayState state = m_playMode.get_state();
    if (state != PlayState::Play && state != PlayState::Simulate) return;
    Scene* playScene = m_playMode.get_active_scene();
    if (!playScene) return;

    // Wicked-port runtime: force fields push/pull bodies within range;
    // springs pull their body back toward the authored rest anchor. Both run
    // before the solver step so the forces feed this frame's simulation.
    for (const auto& [id, ff] : playScene->forceFieldComponents) {
        if (!ff.enabled) continue;
        const auto fit = playScene->transformComponents.find(id);
        const glm::vec3 center = (fit != playScene->transformComponents.end())
                                     ? fit->second.position
                                     : glm::vec3(0.0f);
        glm::vec3 forward(0.0f, 0.0f, 1.0f);
        if (fit != playScene->transformComponents.end()) {
            forward = glm::quat(glm::radians(fit->second.rotation)) * glm::vec3(0.0f, 0.0f, 1.0f);
        }
        for (const auto& [bid, handle] : m_playBodies) {
            Physics::RigidBody* body = m_playPhysics.body(handle);
            if (!body || !body->dynamic()) continue;
            const glm::vec3 delta = body->position - center;
            const float dist = glm::length(delta);
            if (dist > ff.range) continue;
            const float falloff = 1.0f - (ff.range > 0.01f ? dist / ff.range : 0.0f);
            glm::vec3 force(0.0f);
            switch (ff.type) {
                case ForceFieldType::Gravity:
                    force = glm::vec3(0.0f, -ff.strength * 9.81f, 0.0f);
                    break;
                case ForceFieldType::Push:
                    force = forward * (ff.strength * 40.0f);
                    break;
                case ForceFieldType::Wind:
                    force = forward * (ff.strength * 15.0f);
                    break;
                case ForceFieldType::Vortex: {
                    const glm::vec3 tangent = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), delta));
                    force = tangent * (ff.strength * 30.0f) + glm::vec3(0.0f, ff.strength * 2.0f, 0.0f);
                    break;
                }
            }
            m_playPhysics.add_force(handle, force * falloff);
        }
    }
    for (const auto& [id, sp] : playScene->springComponents) {
        if (sp.disabled || !sp.enabled) continue;
        const auto restIt = m_springRests.find(id);
        const auto bodyIt = m_playBodies.find(id);
        if (restIt == m_springRests.end() || bodyIt == m_playBodies.end()) continue;
        Physics::RigidBody* body = m_playPhysics.body(bodyIt->second);
        if (!body || !body->dynamic()) continue;
        const glm::vec3 force = (restIt->second - body->position) * (12.0f * sp.stiffness)
                              - body->linearVelocity * (2.0f * sp.drag);
        m_playPhysics.add_force(bodyIt->second, force);
    }
    // Wicked-port runtime: constraints as soft point-to-point springs between
    // the entity's anchor and the target body's anchor (fixed when the other
    // entity is missing). Broken when the required force exceeds breakForce.
    for (const auto& [id, cn] : playScene->constraintComponents) {
        if (!cn.enabled) continue;
        auto restIt = m_constraintRests.find(id);
        const auto bodyIt = m_playBodies.find(id);
        if (restIt == m_constraintRests.end() || bodyIt == m_playBodies.end()) continue;
        ConstraintRest& rest = restIt->second;
        if (rest.broken) continue;
        Physics::RigidBody* bodyA = m_playPhysics.body(bodyIt->second);
        if (!bodyA || !bodyA->dynamic()) continue;
        Physics::RigidBody* bodyB = nullptr;
        Physics::BodyHandle bodyBHandle = Physics::InvalidBody;
        if (const auto bodyBIt = m_playBodies.find(cn.otherEntity); bodyBIt != m_playBodies.end()) {
            bodyB = m_playPhysics.body(bodyBIt->second);
            bodyBHandle = bodyBIt->second;
        }
        const glm::vec3 anchorA = bodyA->position + cn.anchor;
        glm::vec3 anchorB = rest.anchorB;
        if (bodyB && bodyB->dynamic()) anchorB = bodyB->position + cn.anchor;
        const glm::vec3 delta = anchorB - anchorA;
        const float dist = glm::length(delta);
        glm::vec3 force(0.0f);
        if (dist > 1e-5f) {
            const glm::vec3 dir = delta / dist;
            if (cn.type == ConstraintType::Spring) {
                force = dir * ((dist - rest.restLength) * 25.0f);
            } else {
                // Fixed / Hinge / Point: pull the anchors together (stiff).
                force = dir * (dist * 40.0f);
            }
        }
        force -= bodyA->linearVelocity * 0.8f; // axial damping, no ringing
        if (cn.breakForce > 0.0f && glm::length(force) > cn.breakForce) {
            rest.broken = true;
            continue;
        }
        m_playPhysics.add_force(bodyIt->second, force);
        if (bodyB && bodyB->dynamic() && bodyBHandle != Physics::InvalidBody) {
            m_playPhysics.add_force(bodyBHandle, -force);
        }
    }

    m_playPhysics.step(deltaTime);
    for (const auto& [id, handle] : m_playBodies) {
        Physics::RigidBody* body = m_playPhysics.body(handle);
        if (!body) continue;
        // NaN/inf guard: a solver blow-up (e.g. a body catapulted by a broken
        // constraint) must never poison a transform, or the view/projection
        // matrices become NaN and the whole viewport renders black.
        const auto finite3 = [](const glm::vec3& v) {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        };
        const auto finiteQ = [](const glm::quat& q) {
            return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
        };
        if (!finite3(body->position) || !finiteQ(body->rotation)) {
            // Reset the body to a sane resting state instead of propagating NaN.
            body->position = glm::vec3(0.0f, 1.0f, 0.0f);
            body->linearVelocity = glm::vec3(0.0f);
            body->angularVelocity = glm::vec3(0.0f);
            body->rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            continue;
        }
        auto tit = playScene->transformComponents.find(id);
        if (tit == playScene->transformComponents.end()) continue;
        tit->second.position = body->position;
        tit->second.rotation = glm::degrees(glm::eulerAngles(body->rotation));
    }

    // Wicked-port runtime: spline followers drive their entity along the
    // Catmull-Rom path (looped) in play mode; kinematic bodies follow too.
    for (const auto& [id, sp] : playScene->splineComponents) {
        if (!sp.enabled || sp.points.size() < 2) continue;
        auto tit = playScene->transformComponents.find(id);
        if (tit == playScene->transformComponents.end()) continue;
        auto [progIt, inserted] = m_splineProgress.try_emplace(id, 0.0f);
        (void)inserted;
        float total = 0.0f;
        for (size_t i = 1; i < sp.points.size(); ++i) {
            total += glm::length(sp.points[i] - sp.points[i - 1]);
        }
        if (total < 1e-4f) continue;
        constexpr float kSplineSpeed = 2.0f; // m/s
        progIt->second += kSplineSpeed * deltaTime / total;
        if (sp.looped) {
            progIt->second -= std::floor(progIt->second);
        } else {
            progIt->second = glm::clamp(progIt->second, 0.0f, 1.0f);
        }
        const float t = progIt->second * static_cast<float>(sp.points.size() - 1);
        const size_t i = std::min<size_t>(static_cast<size_t>(t), sp.points.size() - 2);
        const float f = t - static_cast<float>(i);
        const glm::vec3& p0 = sp.points[i > 0 ? i - 1 : i];
        const glm::vec3& p1 = sp.points[i];
        const glm::vec3& p2 = sp.points[i + 1];
        const glm::vec3& p3 = sp.points[std::min(i + 2, sp.points.size() - 1)];
        const float f2 = f * f, f3 = f2 * f;
        const glm::vec3 pos = 0.5f * ((2.0f * p1) + (-p0 + p2) * f
            + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * f2
            + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * f3);
        tit->second.position = pos;
        const auto bodyIt = m_playBodies.find(id);
        if (bodyIt != m_playBodies.end()) {
            Physics::RigidBody* body = m_playPhysics.body(bodyIt->second);
            if (body) {
                body->position = pos;
                body->linearVelocity = glm::vec3(0.0f);
                body->angularVelocity = glm::vec3(0.0f);
            }
        }
    }

    // Play-world animation (Animation/Timeline/IK/Retarget): the editors now
    // Apply to the scene, so the runtime advances timelines, samples clips
    // into bone-entity transforms, solves IK chains and mirrors retarget
    // mappings. Runs after physics so animation wins over the solver.
    tick_animation_runtime(playScene, deltaTime);

    // Hot reload: recompile + swap the program when the .script changes on
    // disk (variables survive — load() keeps the variable map).
    if (m_playScriptLoaded) {
        std::string reloadError;
        if (m_playScriptReloader.reload_if_changed(m_playScript, &reloadError)) {
            std::cout << "[Editor] Script hot-reloaded: " << m_playScriptPath.filename().string()
                      << (reloadError.empty() ? "" : " (" + reloadError + ")") << std::endl;
            m_playScript.start_event("OnStart");
        }
        // Debugger-aware tick: hold on a panel pause or a breakpoint pause;
        // otherwise drive the VM through the debugger so breakpoints halt it
        // and the panel stays in sync (variables/ip/call stack).
        const bool breakpointPaused = m_playScript.status() == VMStatus::Paused;
        if (!m_scriptPauseRequested && !breakpointPaused) {
            if (m_scriptDebuggerAttached) m_scriptDebugger.continue_run(10000, deltaTime);
            else m_playScript.run(deltaTime, 10000);
            if (m_playScript.status() == VMStatus::Paused) return; // hit a breakpoint
            std::vector<std::string> emitted;
            m_playScript.consume_emitted_events(emitted);
            for (const std::string& event : emitted) m_playScript.start_event(event);
        if (m_playScript.has_event("Tick")) {
            m_playScript.start_event("Tick");
            if (m_scriptDebuggerAttached) m_scriptDebugger.continue_run(10000, deltaTime);
            else m_playScript.run(deltaTime, 10000);
        }
    }

    // Play weapons (Fase 8): one WeaponRuntime per WeaponComponent entity,
    // fired with the viewport camera ray against the play physics on SPACE.
    const bool fireHeld = glfwGetKey(m_window, GLFW_KEY_SPACE) == GLFW_PRESS;
    const glm::mat4 camView = m_editorCamera.get_view_matrix();
    const glm::vec3 camFront = glm::normalize(
        glm::vec3(-camView[2][0], -camView[2][1], -camView[2][2]));
    for (const auto& [id, comp] : playScene->weaponComponents) {
        auto it = m_playWeapons.find(id);
        if (it == m_playWeapons.end()) {
            Engine::WeaponDefinition def;
            def.id = id;
            def.name = "Scene Weapon";
            def.fireMode = comp.automatic ? Engine::FireMode::Automatic : Engine::FireMode::Single;
            def.magazineSize = comp.magazineSize;
            def.reserveAmmo = comp.reserveAmmo;
            def.roundsPerMinute = comp.roundsPerMinute;
            def.damage = comp.damage;
            def.range = 120.0f;
            def.spreadDegrees = comp.spreadDegrees;
            def.hitscan = comp.hitscan;
            it = m_playWeapons.emplace(id, Engine::WeaponRuntime(std::move(def))).first;
            it->second.set_raycast([this](const glm::vec3& o, const glm::vec3& d, float maxDist)
                                       -> std::optional<Engine::WeaponHit> {
                const auto hit = m_playPhysics.raycast(o, d, maxDist);
                if (!hit) return std::nullopt;
                Engine::WeaponHit out;
                out.position = hit->point;
                out.normal = hit->normal;
                out.distance = hit->distance;
                return out;
            });
        }
        if (fireHeld) it->second.trigger_pressed(m_editorCamera.position, camFront);
        else it->second.trigger_released();
        it->second.update(deltaTime, m_editorCamera.position, camFront);
    }
    if (fireHeld && !m_playWeaponStatusLogged && !m_playWeapons.empty()) {
        m_playWeaponStatusLogged = true;
        std::cout << "[Editor] Play weapon firing via physics raycast ("
                  << m_playWeapons.size() << " weapon entity/entities)\n";
    }

    // Play particles (Fase 8): keep each emitter at its entity's world
    // position and step the simulation against the play physics.
    for (const auto& [id, emitter] : m_playEmitters) {
        auto* desc = m_playParticles.emitter(emitter);
        if (!desc) continue;
        const auto tit = playScene->transformComponents.find(id);
        const auto pe = playScene->particleEmitterComponents.find(id);
        const glm::vec3 localPos =
            (pe != playScene->particleEmitterComponents.end()) ? pe->second.position : glm::vec3(0.0f);
        desc->position = (tit != playScene->transformComponents.end())
                             ? tit->second.position + localPos
                             : localPos;
    }
    m_playParticles.update(deltaTime, &m_playPhysics);

    // Play vehicles (Fase 8): drive with the arrow keys.
    const bool throttle = glfwGetKey(m_window, GLFW_KEY_UP) == GLFW_PRESS;
    const bool brake = glfwGetKey(m_window, GLFW_KEY_DOWN) == GLFW_PRESS;
    const float steer = (glfwGetKey(m_window, GLFW_KEY_RIGHT) == GLFW_PRESS ? 1.0f : 0.0f) -
                        (glfwGetKey(m_window, GLFW_KEY_LEFT) == GLFW_PRESS ? 1.0f : 0.0f);
    for (auto& [id, vehicle] : m_playVehicles) {
        (void)id;
        Engine::Gameplay::VehicleInput input;
        input.throttle = throttle ? 1.0f : 0.0f;
        input.brake = brake ? 1.0f : 0.0f;
        input.steering = steer;
        vehicle.set_input(input);
        vehicle.update(m_playPhysics, deltaTime);
    }

    // Play missions (Fase 8): step the graph and mirror the live state back
    // to the component. Events emitted by the play script (consume_emitted)
    // are dispatched to the mission system, so authored script events can
    // complete missions.
    m_playMissions.update(deltaTime);
    for (auto& [id, mc] : playScene->missionComponents) {
        const Engine::Gameplay::Mission* mission = m_playMissions.mission("mission_" + id.to_string());
        mc.active = mission && mission->is_active();
    }
    {
        std::vector<std::string> emitted;
        if (m_playScriptLoaded) m_playScript.consume_emitted_events(emitted);
        for (const std::string& event : emitted) m_playMissions.dispatch_event(event);
    }

    // Play dialogues (Fase 8): mirror the playing state.
    for (auto& [id, dc] : playScene->dialogueComponents) {
        dc.playing = m_playDialogues.is_playing();
    }

    // Play audio (Fase 8): keep the listener on the camera and render one
    // mix block per frame so voices advance; drop voices that finished.
    m_playAudio.set_listener(m_editorCamera.position, camFront);
    for (const auto& [id, voice] : m_playVoices) {
        const auto tit = playScene->transformComponents.find(id);
        if (tit != playScene->transformComponents.end()) {
            m_playAudio.set_voice_position(voice, tit->second.position);
        }
        auto ac = playScene->audioComponents.find(id);
        if (ac != playScene->audioComponents.end()) ac->second.playing = m_playAudio.is_active(voice);
    }

    // Play destructibles (Fase 8): weapon hits from this frame apply radial
    // damage (chunks detach with an impulse) and the destroyed flag syncs.
    std::vector<glm::vec3> hitPoints;
    for (const auto& [id, comp] : playScene->weaponComponents) {
        (void)id;
        auto it = m_playWeapons.find(id);
        if (it == m_playWeapons.end()) continue;
        for (const Engine::WeaponHit& hit : it->second.hits()) hitPoints.push_back(hit.position);
        it->second.clear_hits();
    }
    for (auto& [id, runtime] : m_playDestructibles) {
        auto dc = playScene->destructionComponents.find(id);
        for (const glm::vec3& point : hitPoints) {
            runtime.apply_radial_damage(m_playPhysics, point, dc->second.damageRadius, 25.0f, dc->second.damageImpulse);
        }
        if (dc != playScene->destructionComponents.end()) {
            dc->second.destroyed = runtime.fully_destroyed();
        }
    }

    // Play navigation (Fase 8): repath toward the camera entity when the agent
    // arrives (or the target moved), then write the agent position back.
    glm::vec3 target{0.0f};
    bool haveTarget = false;
    for (const auto& [cid, cam] : playScene->cameraComponents) {
        (void)cam;
        const auto tit = playScene->transformComponents.find(cid);
        if (tit != playScene->transformComponents.end()) {
            target = tit->second.position;
            haveTarget = true;
            break;
        }
    }
    if (!haveTarget) target = m_editorCamera.position;
    for (auto& [id, agent] : m_playNavAgents) {
        if (m_playNav &&
            (agent.reached_destination() ||
             glm::distance(agent.position, target) > 1.0f)) {
            engine::navigation::PathResult result;
            if (m_playNav->find_path(agent.position.x, agent.position.y,
                                     agent.position.z, target.x, target.y,
                                     target.z, result) && result.found) {
                std::vector<glm::vec3> points;
                points.reserve(result.waypoints.size() / 3);
                for (std::size_t i = 0; i + 2 < result.waypoints.size(); i += 3) {
                    points.emplace_back(result.waypoints[i], result.waypoints[i + 1],
                                        result.waypoints[i + 2]);
                }
                agent.set_path(std::move(points));
            }
        }
        agent.update(deltaTime);
        auto tit = playScene->transformComponents.find(id);
        if (tit != playScene->transformComponents.end()) tit->second.position = agent.position;
    }
}
}

void EditorApplication::teardown_play_runtime() {
    m_constraintRests.clear();
    m_springRests.clear();
    m_splineProgress.clear();
    m_animStates.clear();
    m_animClips.clear();
    if (m_playGroundBody != Physics::InvalidBody) {
        m_playPhysics.destroy_body(m_playGroundBody);
        m_playGroundBody = Physics::InvalidBody;
    }
    for (const Physics::BodyHandle handle : m_playStaticBodies) {
        m_playPhysics.destroy_body(handle);
    }
    m_playStaticBodies.clear();
    for (const auto& [id, handle] : m_playBodies) {
        (void)id;
        m_playPhysics.destroy_body(handle);
    }
    m_playBodies.clear();        m_playScriptLoaded = false;
    m_scriptDebugger.detach();
    m_scriptDebuggerAttached = false;
    m_scriptPauseRequested = false;
    m_playWeapons.clear();
    m_playWeaponStatusLogged = false;
    for (const auto& [id, handle] : m_playVehicleChassis) {
        (void)id;
        m_playPhysics.destroy_body(handle);
    }
    m_playVehicleChassis.clear();
    m_playVehicles.clear();
    m_playParticles.clear();
    m_playEmitters.clear();
    for (auto& [id, ragdoll] : m_playRagdolls) {
        (void)id;
        ragdoll.destroy(m_playPhysics);
    }
    m_playRagdolls.clear();
    m_playMissions.clear();
    m_playMissionIds.clear();
    m_playDialogues.clear();
    m_playDialogueIds.clear();
    for (const auto& [id, voice] : m_playVoices) {
        (void)id;
        m_playAudio.stop(voice);
    }
    m_playVoices.clear();
    for (auto& [id, runtime] : m_playDestructibles) {
        (void)id;
        runtime.destroy(m_playPhysics);
    }
    m_playDestructibles.clear();
    m_playNavAgents.clear();
    m_playNav.reset();
}

// Captures the offscreen viewport (the previous rendered frame) to a PNG file.
// Runs on the main/render thread at the top of the frame, so it first waits
// for the device to idle, copies the color image into a host-visible staging
// buffer, then encodes via Windows Imaging Component (same tech as the PNG
// decoder). Returns empty on success, or a human-readable error.
std::string EditorApplication::capture_viewport_screenshot(const std::string& path) {
    if (m_offscreen.colorImage == VK_NULL_HANDLE || m_offscreen.width == 0 || m_offscreen.height == 0)
        return "screenshot: viewport not initialized";
    const uint32_t w = m_offscreen.width, h = m_offscreen.height;
    const VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 4;
    // Make sure no frame is in flight before we read the image back.
    vkDeviceWaitIdle(m_device);
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  staging, stagingMemory);
    if (staging == VK_NULL_HANDLE) return "screenshot: staging buffer allocation failed";
    {
        VkCommandBuffer cmd = begin_single_time_commands();
        transition_image_layout(cmd, m_offscreen.colorImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { w, h, 1 };
        vkCmdCopyImageToBuffer(cmd, m_offscreen.colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging, 1, &region);
        transition_image_layout(cmd, m_offscreen.colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        end_single_time_commands(cmd);
    }
    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMemory, 0, size, 0, &mapped);
    if (!mapped) {
        vkDestroyBuffer(m_device, staging, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
        return "screenshot: staging buffer map failed";
    }
    std::vector<uint8_t> rgba(static_cast<size_t>(size));
    std::memcpy(rgba.data(), mapped, static_cast<size_t>(size));
    vkUnmapMemory(m_device, stagingMemory);
    vkDestroyBuffer(m_device, staging, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);

    // ImGui displays the offscreen texture with UV v inverted (uv0=(0,1) →
    // uv1=(1,0)), so the readback's row 0 is the BOTTOM of what the user
    // sees. Flip rows so the saved PNG matches the displayed viewport.
    std::vector<uint8_t> flipped(static_cast<size_t>(size));
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t srcY = h - 1 - y;
        std::memcpy(flipped.data() + static_cast<size_t>(y) * w * 4,
                    rgba.data() + static_cast<size_t>(srcY) * w * 4, w * 4);
    }

    // Encode RGBA -> PNG via WIC (same tech as the PNG decoder).
    static ComPtr<IWICImagingFactory> factory;
    if (!factory) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()))))
            return "screenshot: WIC factory init failed";
    }
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return "screenshot: WIC stream creation failed";
    {
        const std::wstring wide(path.begin(), path.end());
        if (FAILED(stream->InitializeFromFilename(wide.c_str(), GENERIC_WRITE)))
            return "screenshot: cannot open file for writing: " + path;
    }
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)))
        return "screenshot: PNG encoder creation failed";
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return "screenshot: PNG encoder init failed";
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) return "screenshot: PNG frame creation failed";
    if (FAILED(frame->Initialize(props.Get()))) return "screenshot: PNG frame init failed";
    if (FAILED(frame->SetSize(w, h))) return "screenshot: PNG frame size failed";
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->SetPixelFormat(&fmt))) return "screenshot: PNG pixel format failed";
    if (FAILED(frame->WritePixels(h, w * 4, static_cast<UINT>(flipped.size()), flipped.data())))
        return "screenshot: PNG write failed";
    if (FAILED(frame->Commit())) return "screenshot: PNG frame commit failed";
    if (FAILED(encoder->Commit())) return "screenshot: PNG encoder commit failed";
    return std::string();
}

bool EditorApplication::snapshot_ui_frame(VkCommandBuffer cmd, VkImage swapchainImage) {
    // Copia o frame final (viewport + UI ImGui) do swapchain para uma imagem
    // persistente m_uiSnapshot, para /screenshot-ui poder ler (capturar a UI).
    const uint32_t w = m_swapchainExtent.width, h = m_swapchainExtent.height;
    if (w == 0 || h == 0) return false;
    if (m_uiSnapshotImage == VK_NULL_HANDLE || m_uiSnapshotW != w || m_uiSnapshotH != h ||
        m_uiSnapshotFormat != m_swapchainFormat) {
        if (m_uiSnapshotImage != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, m_uiSnapshotView, nullptr);
            vkDestroyImage(m_device, m_uiSnapshotImage, nullptr);
            vkFreeMemory(m_device, m_uiSnapshotMemory, nullptr);
            m_uiSnapshotImage = VK_NULL_HANDLE; m_uiSnapshotView = VK_NULL_HANDLE;
            m_uiSnapshotMemory = VK_NULL_HANDLE; m_uiSnapshotReady = false;
        }
        create_image(w, h, m_swapchainFormat,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     m_uiSnapshotImage, m_uiSnapshotMemory);
        if (m_uiSnapshotImage == VK_NULL_HANDLE) return false;
        m_uiSnapshotView = create_image_view(m_uiSnapshotImage, m_swapchainFormat,
                                             VK_IMAGE_ASPECT_COLOR_BIT);
        m_uiSnapshotW = w; m_uiSnapshotH = h; m_uiSnapshotFormat = m_swapchainFormat;
    }
    transition_image_layout(cmd, swapchainImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    transition_image_layout(cmd, m_uiSnapshotImage, VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageCopy region{};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.srcOffset = { 0, 0, 0 }; region.dstOffset = { 0, 0, 0 };
    region.extent = { w, h, 1 };
    vkCmdCopyImage(cmd, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   m_uiSnapshotImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transition_image_layout(cmd, m_uiSnapshotImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    transition_image_layout(cmd, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);
    m_uiSnapshotReady = true;
    return true;
}

std::string EditorApplication::capture_ui_screenshot(const std::string& path) {
    // Lê a snapshot do frame final (viewport + UI) e grava PNG via WIC.
    // O swapchain não inverte a vertical (ao contrário do offscreen), então
    // nenhum flip é necessário.
    if (!m_uiSnapshotReady || m_uiSnapshotImage == VK_NULL_HANDLE)
        return "screenshot-ui: no frame snapshot (editor sem UI renderizada)";
    const uint32_t w = m_uiSnapshotW, h = m_uiSnapshotH;
    const VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 4;
    vkDeviceWaitIdle(m_device);
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  staging, stagingMemory);
    if (staging == VK_NULL_HANDLE) return "screenshot-ui: staging allocation failed";
    {
        VkCommandBuffer cmd = begin_single_time_commands();
        transition_image_layout(cmd, m_uiSnapshotImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { w, h, 1 };
        vkCmdCopyImageToBuffer(cmd, m_uiSnapshotImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging, 1, &region);
        transition_image_layout(cmd, m_uiSnapshotImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        end_single_time_commands(cmd);
    }
    void* mapped = nullptr;
    vkMapMemory(m_device, stagingMemory, 0, size, 0, &mapped);
    if (!mapped) {
        vkDestroyBuffer(m_device, staging, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
        return "screenshot-ui: staging map failed";
    }
    std::vector<uint8_t> rgba(static_cast<size_t>(size));
    std::memcpy(rgba.data(), mapped, static_cast<size_t>(size));
    vkUnmapMemory(m_device, stagingMemory);
    vkDestroyBuffer(m_device, staging, nullptr);
    vkFreeMemory(m_device, stagingMemory, nullptr);

    static ComPtr<IWICImagingFactory> factory;
    if (!factory) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()))))
            return "screenshot-ui: WIC factory init failed";
    }
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return "screenshot-ui: WIC stream creation failed";
    { const std::wstring wide(path.begin(), path.end());
      if (FAILED(stream->InitializeFromFilename(wide.c_str(), GENERIC_WRITE)))
          return "screenshot-ui: cannot open file: " + path; }
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)))
        return "screenshot-ui: PNG encoder creation failed";
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return "screenshot-ui: PNG encoder init failed";
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) return "screenshot-ui: PNG frame creation failed";
    if (FAILED(frame->Initialize(props.Get()))) return "screenshot-ui: PNG frame init failed";
    if (FAILED(frame->SetSize(w, h))) return "screenshot-ui: PNG frame size failed";
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->SetPixelFormat(&fmt))) return "screenshot-ui: PNG pixel format failed";
    if (FAILED(frame->WritePixels(h, w * 4, static_cast<UINT>(rgba.size()), rgba.data())))
        return "screenshot-ui: PNG write failed";
    if (FAILED(frame->Commit())) return "screenshot-ui: PNG frame commit failed";
    if (FAILED(encoder->Commit())) return "screenshot-ui: PNG encoder commit failed";
    return std::string();
}


} // namespace Engine
