#include "EditorApplication.hpp"
#include "frontend/FontAwesomeV6.h"
#include "frontend/IconsFontAwesome6.h"
#include "frontend/liberation_sans.h"
#include "frontend/ForgeTheme.hpp"
#include "frontend/ForgeWidgets.hpp"
#include "engine/compression/ICompressionProvider.hpp"
#include "../engine/assets/GltfGeometry.hpp"
#include "../engine/animation/AnimationAssets.hpp"
#include "../engine/rendering/vulkan/MaterialPipeline.hpp"
#include "../engine/audio/AudioRuntime.hpp"
#include "../engine/audio/OggDecoder.hpp"
#include "../engine/gameplay/DialogueSystem.hpp"
#include "../engine/gameplay/DestructionRuntime.hpp"
#include "../engine/gameplay/MissionSystem.hpp"
#include "engine/navigation/INavigationProvider.hpp"
#include <array>
#include <random>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <type_traits>
#include <stdexcept>
#include <algorithm>
#include <functional>
#include <unordered_set>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <fstream>
#include <chrono>
#include <ctime>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <VkBootstrap.h>
#include <miniaudio.h>
#include <thread>

namespace {
glm::mat4 model_from_transform(const Engine::TransformComponent& t) {
    glm::mat4 model(1.0f);
    model = glm::translate(model, t.position);
    model = glm::rotate(model, glm::radians(t.rotation.z), glm::vec3(0, 0, 1));
    model = glm::rotate(model, glm::radians(t.rotation.y), glm::vec3(0, 1, 0));
    model = glm::rotate(model, glm::radians(t.rotation.x), glm::vec3(1, 0, 0));
    model = glm::scale(model, t.scale);
    return model;
}
void push_constants(VkCommandBuffer cmd, VkPipelineLayout layout, const glm::mat4& mvp, const glm::vec4& color) {
    const Engine::ScenePushConstants pc{ mvp, color };
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       static_cast<uint32_t>(sizeof(pc)), &pc);
}
void set_viewport_scissor(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{ { 0, 0 }, { w, h } };
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}
void draw_indexed_cube(VkCommandBuffer cmd, VkPipelineLayout layout, const VkBuffer& vb, const VkBuffer& ib,
                       uint32_t indexCount, const glm::mat4& mvp, const glm::vec4& color) {
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
    push_constants(cmd, layout, mvp, color);
    vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
}
void draw_indexed_editor_mesh(VkCommandBuffer cmd, VkPipelineLayout layout, const VkBuffer& vb,
                              const VkBuffer& ib, uint32_t indexCount, const glm::mat4& mvp,
                              const glm::vec4& color) {
    draw_indexed_cube(cmd, layout, vb, ib, indexCount, mvp, color);
}
} // namespace

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <wincodec.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

namespace Engine {

// ===========================================================================
// Play Mode, Control API & Gizmo (split from EditorApplication.cpp)
// ===========================================================================
void EditorApplication::setup_play_runtime() {
    teardown_play_runtime();
    Scene* playScene = m_playMode.get_active_scene();
    if (!playScene) return;
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

    // Wicked-port runtime (formerly TODO(frontend-port)): constraints run as
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
        for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
            if (asset.type == AssetType::Audio && asset.sourcePath == ac.clipPath) {
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
    void* data = nullptr;
    vkMapMemory(m_device, sim.vb.memory, 0, size, 0, &data);
    std::memcpy(data, verts.data(), static_cast<size_t>(size));
    vkUnmapMemory(m_device, sim.vb.memory);
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
    void* data = nullptr;
    vkMapMemory(m_device, sim.vb.memory, 0, vs, 0, &data);
    std::memcpy(data, verts.data(), static_cast<size_t>(vs));
    vkUnmapMemory(m_device, sim.vb.memory);
    vkMapMemory(m_device, sim.ib.memory, 0, is, 0, &data);
    std::memcpy(data, sim.indices.data(), static_cast<size_t>(is));
    vkUnmapMemory(m_device, sim.ib.memory);
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
        void* data = nullptr;
        vkMapMemory(m_device, it->second.vb.memory, 0, vs, 0, &data);
        std::memcpy(data, verts.data(), static_cast<size_t>(vs));
        vkUnmapMemory(m_device, it->second.vb.memory);
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


void EditorApplication::handle_control_command(const std::string& cmd) {
    if (cmd == "play" && m_playMode.get_state() == PlayState::Edit) {
        m_playMode.start_play(m_editorScene.get());
        setup_play_runtime();
        std::cout << "[ControlApi] play started" << std::endl;
    } else if (cmd == "pause" && m_playMode.get_state() == PlayState::Play) {
        m_playMode.pause_play();
        std::cout << "[ControlApi] paused" << std::endl;
    } else if (cmd == "resume" && m_playMode.get_state() == PlayState::Pause) {
        m_playMode.pause_play();
        std::cout << "[ControlApi] resumed" << std::endl;
    } else if (cmd == "step" && m_playMode.get_state() == PlayState::Pause) {
        m_stepRequested = true;
        std::cout << "[ControlApi] step" << std::endl;
    } else if (cmd == "stop" && m_playMode.get_state() != PlayState::Edit) {
        teardown_play_runtime();
        m_playMode.stop_play();
        m_selectedEntity = Entity();
        m_editorGui.select_entity(m_selectedEntity);
        std::cout << "[ControlApi] stopped" << std::endl;
    } else if (cmd.rfind("zoom ", 0) == 0) {
        const float amount = std::stof(cmd.substr(5));
        m_editorCamera.orbitDistance =
            glm::clamp(m_editorCamera.orbitDistance * (1.0f - amount), 0.5f, 5000.0f);
        recompute_editor_camera_position();
        std::cout << "[ControlApi] zoom " << amount << " -> " << m_editorCamera.orbitDistance << std::endl;
    } else if (cmd.rfind("move ", 0) == 0) {
        float fx = 0.0f, ry = 0.0f, uz = 0.0f;
        std::istringstream ss(cmd.substr(5));
        ss >> fx >> ry >> uz;
        m_editorCamera.orbitTarget +=
            m_editorCamera.get_front() * fx +
            m_editorCamera.get_right() * ry +
            m_editorCamera.get_up() * uz;
        recompute_editor_camera_position();
        std::cout << "[ControlApi] move " << fx << " " << ry << " " << uz << std::endl;
    } else if (cmd.rfind("turn ", 0) == 0) {
        float yawDeg = 0.0f, pitchDeg = 0.0f;
        std::istringstream ss(cmd.substr(5));
        ss >> yawDeg >> pitchDeg;
        m_editorCamera.yaw += yawDeg;
        m_editorCamera.pitch = glm::clamp(m_editorCamera.pitch + pitchDeg, -89.0f, 89.0f);
        recompute_editor_camera_position();
        std::cout << "[ControlApi] turn " << yawDeg << " " << pitchDeg << std::endl;
    } else if (cmd.rfind("terrain ", 0) == 0) {
        float scale = 1.0f, amount = 0.5f, falloff = 0.4f;
        int octaves = 4;
        std::istringstream ss(cmd.substr(8));
        ss >> scale >> octaves >> amount >> falloff;
        generate_terrain_mesh(TerrainParams{ scale, octaves, amount, falloff });
        std::cout << "[ControlApi] terrain scale=" << scale << " octaves=" << octaves
                  << " amount=" << amount << " falloff=" << falloff << std::endl;
    } else if (cmd.rfind("graphics ", 0) == 0) {
        int vsyncInt = 1, quality = 2;
        std::istringstream ss(cmd.substr(9));
        ss >> vsyncInt >> quality;
        apply_graphics_settings(vsyncInt != 0, quality);
        std::cout << "[ControlApi] graphics vsync=" << vsyncInt << " quality=" << quality << std::endl;
    } else if (cmd == "save-settings") {
        save_settings();
        std::cout << "[ControlApi] save-settings" << std::endl;
    } else if (cmd.rfind("project ", 0) == 0) {
        const std::string result = create_project(cmd.substr(8), "");
        std::cout << "[ControlApi] project -> " << result << std::endl;
    } else if (cmd.rfind("mesh ", 0) == 0) {
        int mode = 0;
        std::istringstream ss(cmd.substr(5));
        ss >> mode;
        const std::string result = apply_mesh_normals(mode);
        std::cout << "[ControlApi] mesh " << mode << " -> " << result << std::endl;
    } else if (cmd == "simulate" && m_playMode.get_state() == PlayState::Edit) {
        m_playMode.start_simulate(m_editorScene.get());
        setup_play_runtime();
        std::cout << "[ControlApi] simulate started" << std::endl;
    } else if (cmd == "new-scene") {
        init_default_scene();
        std::cout << "[ControlApi] new scene" << std::endl;
    } else if (cmd.rfind("open-scene ", 0) == 0) {
        // Resolve relative paths against the source root: the editor process
        // cwd is not guaranteed to be the engine folder.
        std::string scenePath = cmd.substr(11);
        std::filesystem::path rel(scenePath);
        if (rel.is_relative()) {
            const auto abs = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / rel;
            if (std::filesystem::exists(abs)) scenePath = abs.string();
        }
        load_scene_file(scenePath);
        // API-driven scene open must leave the launcher hub: the control-API
        // drain and play runtime are gated on !m_inLauncherMode.
        m_inLauncherMode = false;
        std::cout << "[ControlApi] open scene '" << scenePath << "'" << std::endl;
    } else if (cmd == "save-scene") {
        // API-safe: never open a blocking native dialog from the HTTP thread
        // path (that would wedge the main loop). If there is no active scene
        // path yet, fall back to a timestamped file in the scenes folder.
        if (!m_editorScene) {
            std::cout << "[ControlApi] save-scene: no scene" << std::endl;
        } else if (!m_activeScenePath.empty()) {
            if (m_editorScene->save_to_file(m_activeScenePath)) {
                std::cout << "[ControlApi] scene saved: " << m_activeScenePath << std::endl;
            } else {
                std::cerr << "[ControlApi] save-scene failed: " << m_activeScenePath << std::endl;
            }
        } else {
            const auto scenesDir = std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "assets" / "scenes";
            std::error_code ec;
            std::filesystem::create_directories(scenesDir, ec);
            const std::string stamp = std::to_string(static_cast<long long>(std::time(nullptr)));
            const std::filesystem::path fallback = scenesDir / ("api_" + stamp + ".scene");
            if (m_editorScene->save_to_file(fallback.string())) {
                m_activeScenePath = fallback.string();
                std::cout << "[ControlApi] scene saved (new): " << m_activeScenePath << std::endl;
            } else {
                std::cerr << "[ControlApi] save-scene failed: " << fallback << std::endl;
            }
        }
    } else if (cmd.rfind("focus ", 0) == 0) {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        std::istringstream ss(cmd.substr(6));
        ss >> x >> y >> z;
        m_editorCamera.orbitTarget = glm::vec3(x, y, z);
        recompute_editor_camera_position();
        std::cout << "[ControlApi] focus " << x << " " << y << " " << z << std::endl;
    } else if (cmd.rfind("add-entity ", 0) == 0) {
        if (!m_editorScene) { std::cout << "[ControlApi] no scene" << std::endl; return; }
        const std::string type = cmd.substr(11);
        const auto create = [&](const char* name) {
            Entity e = m_editorScene->create_entity(name);
            m_selectedEntity = e;
            m_editorGui.select_entity(e);
            return e;
        };
        Entity e;
        if (type == "empty") e = create("Novo Objeto");
        else if (type == "cube") { e = create("Cubo 3D"); if (e.is_valid()) m_editorScene->meshRendererComponents[e.get_id()] = MeshRendererComponent{}; }
        else if (type == "camera") { e = create("Câmera"); if (e.is_valid()) m_editorScene->cameraComponents[e.get_id()] = CameraComponent{}; }
        else if (type == "sun") { e = create("Luz do Sol"); if (e.is_valid()) m_editorScene->lightComponents[e.get_id()] = LightComponent{}; }
        else if (type == "point") { e = create("Luz de Lâmpada"); if (e.is_valid()) m_editorScene->lightComponents[e.get_id()] = LightComponent{ glm::vec3(1.0f, 0.8f, 0.4f), 5000.0f, 15.0f, true }; }
        else if (type == "spot") { e = create("Luz Spot"); if (e.is_valid()) m_editorScene->lightComponents[e.get_id()] = LightComponent{ glm::vec3(0.2f, 0.5f, 1.0f), 4000.0f, 18.0f, true, LightType::Spot }; }
        else if (type == "area") { e = create("Luz de Área"); if (e.is_valid()) m_editorScene->lightComponents[e.get_id()] = LightComponent{ glm::vec3(1.0f, 0.4f, 0.9f), 1500.0f, 20.0f, true, LightType::Area }; }
        else if (type == "particles") { e = create("Emissor de Partículas"); if (e.is_valid()) m_editorScene->particleEmitterComponents[e.get_id()] = ParticleEmitterComponent{}; }
        else if (type == "audio") { e = create("Fonte de Áudio"); if (e.is_valid()) m_editorScene->audioComponents[e.get_id()] = AudioComponent{}; }
        else if (type == "rigidbody") { e = create("Corpo Rígido"); if (e.is_valid()) m_editorScene->rigidbodyComponents[e.get_id()] = RigidbodyComponent{}; }
        else if (type == "vehicle") { e = create("Veículo"); if (e.is_valid()) m_editorScene->vehicleComponents[e.get_id()] = VehicleComponent{}; }
        else if (type == "destructible") { e = create("Destrutível"); if (e.is_valid()) m_editorScene->destructionComponents[e.get_id()] = DestructionComponent{}; }
        else if (type == "navagent") { e = create("Agente de Navegação"); if (e.is_valid()) m_editorScene->navigationComponents[e.get_id()] = NavigationComponent{}; }
        else if (type == "mission") { e = create("Missão"); if (e.is_valid()) m_editorScene->missionComponents[e.get_id()] = MissionComponent{}; }
        else if (type == "dialogue") { e = create("Diálogo"); if (e.is_valid()) m_editorScene->dialogueComponents[e.get_id()] = DialogueComponent{}; }
#if VC_ENABLE_VOXEL_PLUGIN
        else if (type == "voxelworld") { e = create("Mundo de Blocos"); if (e.is_valid()) m_editorScene->voxelVolumeComponents[e.get_id()] = VoxelVolumeComponent{}; }
#endif
        std::cout << "[ControlApi] add-entity '" << type << "' -> "
                  << (e.is_valid() ? e.get_id().to_string() : "unknown type") << std::endl;
    } else if (cmd.rfind("add-component ", 0) == 0) {
        std::istringstream ss(cmd.substr(14));
        std::string uuidStr, type;
        ss >> uuidStr >> type;
        const UUID id = UUID::from_string(uuidStr);
        if (!m_editorScene || !m_editorScene->get_entities().contains(id)) {
            std::cout << "[ControlApi] add-component: entity not found" << std::endl;
            return;
        }
        Scene* scene = m_editorScene.get();
        if (type == "light") scene->lightComponents[id] = LightComponent{};
        else if (type == "camera") scene->cameraComponents[id] = CameraComponent{};
        else if (type == "mesh") scene->meshRendererComponents[id] = MeshRendererComponent{};
        else if (type == "material") scene->materialComponents[id] = MaterialComponent{};
        else if (type == "rigidbody") scene->rigidbodyComponents[id] = RigidbodyComponent{};
        else if (type == "weapon") scene->weaponComponents[id] = WeaponComponent{};
        else if (type == "vehicle") scene->vehicleComponents[id] = VehicleComponent{};
        else if (type == "ragdoll") scene->ragdollComponents[id] = RagdollComponent{};
        else if (type == "destructible") scene->destructionComponents[id] = DestructionComponent{};
        else if (type == "navigation") scene->navigationComponents[id] = NavigationComponent{};
        else if (type == "particle") scene->particleEmitterComponents[id] = ParticleEmitterComponent{};
        else if (type == "audio") scene->audioComponents[id] = AudioComponent{};
        else if (type == "mission") scene->missionComponents[id] = MissionComponent{};
        else if (type == "dialogue") scene->dialogueComponents[id] = DialogueComponent{};
        else if (type == "animation") scene->animationComponents[id] = AnimationComponent{};
        else if (type == "timeline") scene->timelineComponents[id] = TimelineComponent{};
        else if (type == "ik") scene->ikComponents[id] = IKComponent{};
        else if (type == "retarget") scene->retargetComponents[id] = RetargetComponent{};
#if VC_ENABLE_VOXEL_PLUGIN
        else if (type == "voxel") scene->voxelVolumeComponents[id] = VoxelVolumeComponent{};
#endif
        else { std::cout << "[ControlApi] add-component: unknown type '" << type << "'" << std::endl; return; }
        std::cout << "[ControlApi] add-component " << type << " on " << uuidStr << std::endl;
    } else if (cmd.rfind("delete-entity ", 0) == 0) {
        const UUID id = UUID::from_string(cmd.substr(14));
        if (m_editorScene && m_editorScene->get_entities().contains(id)) {
            m_editorScene->destroy_entity(id);
            if (m_selectedEntity.is_valid() && m_selectedEntity.get_id() == id) m_selectedEntity = Entity();
            std::cout << "[ControlApi] deleted entity" << std::endl;
        } else {
            std::cout << "[ControlApi] delete-entity: not found" << std::endl;
        }
    } else if (cmd.rfind("rename-entity ", 0) == 0) {
        std::istringstream ss(cmd.substr(14));
        std::string uuidStr, name;
        ss >> uuidStr;
        std::getline(ss, name);
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        const UUID id = UUID::from_string(uuidStr);
        if (m_editorScene && m_editorScene->get_entities().contains(id) && !name.empty()) {
            m_editorScene->rename_entity(id, name);
            std::cout << "[ControlApi] renamed to '" << name << "'" << std::endl;
        } else {
            std::cout << "[ControlApi] rename-entity: not found or empty name" << std::endl;
        }
    } else if (cmd.rfind("select ", 0) == 0) {
        const UUID id = UUID::from_string(cmd.substr(7));
        if (m_editorScene && m_editorScene->get_entities().contains(id)) {
            m_selectedEntity = Entity();
            m_selectedEntity = m_editorScene->get_entities().at(id);
            m_editorGui.select_entity(m_selectedEntity);
            std::cout << "[ControlApi] selected " << id.to_string() << std::endl;
        } else {
            std::cout << "[ControlApi] select: entity not found" << std::endl;
        }
    } else if (cmd.rfind("select-name ", 0) == 0) {
        if (!m_editorScene) return;
        const std::string name = cmd.substr(12);
        for (const auto& [id, entity] : m_editorScene->get_entities()) {
            if (entity.get_name() == name || entity.get_name().find(name) != std::string::npos) {
                m_selectedEntity = Entity();
                m_selectedEntity = m_editorScene->get_entities().at(id);
                m_editorGui.select_entity(m_selectedEntity);
                std::cout << "[ControlApi] selected '" << entity.get_name() << "'" << std::endl;
                return;
            }
        }
        std::cout << "[ControlApi] select-name: no match" << std::endl;
    } else if (cmd.rfind("set-transform ", 0) == 0) {
        // Field-masked PATCH (see EditorApplication.cpp): <uuid> <mask> + 9 floats.
        std::istringstream ss(cmd.substr(14));
        std::string uuidStr, maskStr;
        ss >> uuidStr >> maskStr;
        const UUID id = UUID::from_string(uuidStr);
        std::vector<float> values;
        float v;
        while (ss >> v) values.push_back(v);
        auto it = m_editorScene ? m_editorScene->transformComponents.find(id) : m_editorScene->transformComponents.end();
        if (it == m_editorScene->transformComponents.end()) {
            std::cout << "[ControlApi] set-transform: entity not found" << std::endl;
        } else if (values.size() < 9) {
            std::cout << "[ControlApi] set-transform: expected 9 floats" << std::endl;
        } else {
            if (maskStr.size() > 0 && maskStr[0] == '1')
                it->second.position = glm::vec3(values[0], values[1], values[2]);
            if (maskStr.size() > 1 && maskStr[1] == '1')
                it->second.rotation = glm::vec3(values[3], values[4], values[5]);
            if (maskStr.size() > 2 && maskStr[2] == '1')
                it->second.scale = glm::vec3(values[6], values[7], values[8]);
            std::cout << "[ControlApi] transform set (mask " << maskStr << ")" << std::endl;
        }
    } else if (cmd.rfind("gizmo ", 0) == 0) {
        const std::string mode = cmd.substr(6);
        if (mode == "select") m_gizmoMode = GizmoMode::Select;
        else if (mode == "move") m_gizmoMode = GizmoMode::Translate;
        else if (mode == "rotate") m_gizmoMode = GizmoMode::Rotate;
        else if (mode == "scale") m_gizmoMode = GizmoMode::Scale;
        std::cout << "[ControlApi] gizmo " << mode << std::endl;
    } else if (cmd.rfind("gizmo-space ", 0) == 0) {
        m_gizmoLocal = cmd.substr(12) == "local";
        std::cout << "[ControlApi] gizmo-space " << (m_gizmoLocal ? "local" : "world") << std::endl;
    } else if (cmd.rfind("snap ", 0) == 0) {
        m_snapTranslate = std::max(0.0f, std::stof(cmd.substr(5)));
        std::cout << "[ControlApi] snap " << m_snapTranslate << std::endl;
    } else if (cmd.rfind("import ", 0) == 0) {
        if (m_assetPipeline) {
            const std::filesystem::path cookedRoot =
                std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "DerivedDataCache";
            const ImportResult result = m_assetPipeline->import({ cmd.substr(7), cookedRoot, 1 });
            std::cout << "[ControlApi] import -> " << (result ? "ok" : result.error) << std::endl;
        }
    } else if (cmd.rfind("import-pack ", 0) == 0) {
        const size_t count = import_texture_pack(std::filesystem::path(cmd.substr(12)));
        std::cout << "[ControlApi] import-pack -> " << count << " assets imported" << std::endl;
    } else if (cmd.rfind("block-model ", 0) == 0) {
        const UUID texId = UUID::from_string(cmd.substr(12));
        const auto meta = m_assetRegistry.find(texId);
        if (meta && meta->type == AssetType::Texture) {
            create_block_asset(*meta);
            std::cout << "[ControlApi] block model created" << std::endl;
        } else {
            std::cout << "[ControlApi] block-model: texture not found" << std::endl;
        }
    } else if (cmd.rfind("spawn-block ", 0) == 0) {
        const UUID blockId = UUID::from_string(cmd.substr(12));
        spawn_block_entity(blockId, m_editorCamera.position + m_editorCamera.get_front() * 2.0f);
        std::cout << "[ControlApi] spawn-block " << blockId.to_string() << std::endl;
    } else if (cmd.rfind("spawn-character ", 0) == 0) {
        const UUID texId = UUID::from_string(cmd.substr(16));
        const auto meta = m_assetRegistry.find(texId);
        if (meta && meta->type == AssetType::Texture && is_character_texture(*meta)) {
            spawn_character_entity(texId, m_editorCamera.position + m_editorCamera.get_front() * 2.0f);
            std::cout << "[ControlApi] spawn-character " << texId.to_string() << std::endl;
        } else {
            std::cout << "[ControlApi] spawn-character: skin texture not found" << std::endl;
        }
    } else if (cmd.rfind("layer ", 0) == 0) {
        // layer {uuid} {name} — sets the entity's layer name.
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(6));
        std::string uuidStr, name;
        ss >> uuidStr;
        std::getline(ss, name);
        while (!name.empty() && (name.front() == ' ')) name.erase(name.begin());
        if (scene && !uuidStr.empty() && !name.empty()) {
            const UUID id = UUID::from_string(uuidStr);
            scene->layerComponents[id].name = name;
            std::cout << "[ControlApi] layer " << uuidStr << " -> '" << name << "'" << std::endl;
        }
    } else if (cmd.rfind("layer-vis ", 0) == 0) {
        // layer-vis {name} {0|1} — show/hide every entity on that layer.
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(10));
        std::string name;
        int visible = 1;
        std::getline(ss, name, '|');
        ss >> visible;
        while (!name.empty() && name.back() == ' ') name.pop_back();
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        if (scene && !name.empty()) {
            for (auto& [id, lc] : scene->layerComponents) {
                if (lc.name == name) lc.visible = visible != 0;
            }
            std::cout << "[ControlApi] layer-vis '" << name << "' visible=" << visible << std::endl;
        }
    } else if (cmd.rfind("decal-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(10));
        std::string uuidStr, texture;
        ss >> uuidStr;
        std::getline(ss, texture);
        while (!texture.empty() && texture.front() == ' ') texture.erase(texture.begin());
        if (scene && !uuidStr.empty()) {
            DecalComponent dec;
            dec.texturePath = texture;
            scene->decalComponents[UUID::from_string(uuidStr)] = dec;
            std::cout << "[ControlApi] decal-add " << uuidStr << " texture='" << texture << "'" << std::endl;
        }
    } else if (cmd.rfind("hair-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(9));
        if (scene && id.is_valid()) {
            scene->hairParticleComponents[id] = HairParticleComponent{};
            std::cout << "[ControlApi] hair-add " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("softbody-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(13));
        if (scene && id.is_valid()) {
            scene->softBodyComponents[id] = SoftBodyComponent{};
            std::cout << "[ControlApi] softbody-add " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("env-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(8));
        if (scene && id.is_valid()) {
            scene->envProbeComponents[id] = EnvProbeComponent{};
            std::cout << "[ControlApi] env-add " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("env-capture ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(12));
        if (scene && scene->envProbeComponents.contains(id)) {
            scene->envProbeComponents[id].captureRequested = true;
            std::cout << "[ControlApi] env-capture " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("paint-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(10));
        if (scene && id.is_valid()) {
            scene->paintComponents[id] = PaintComponent{};
            std::cout << "[ControlApi] paint-add " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("paint-mode ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(11));
        std::string uuidStr;
        int mode = 0;
        ss >> uuidStr >> mode;
        const UUID id = UUID::from_string(uuidStr);
        if (scene && scene->paintComponents.contains(id)) {
            scene->paintComponents[id].paintMode = mode != 0;
            std::cout << "[ControlApi] paint-mode " << uuidStr << " " << mode << std::endl;
        }
    } else if (cmd.rfind("paint-color ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(12));
        std::string uuidStr;
        float r = 1.0f, g = 0.3f, b = 0.22f;
        ss >> uuidStr >> r >> g >> b;
        const UUID id = UUID::from_string(uuidStr);
        if (scene && scene->paintComponents.contains(id)) {
            scene->paintComponents[id].brushColor = { r, g, b };
            std::cout << "[ControlApi] paint-color " << uuidStr << " " << r << " " << g << " " << b << std::endl;
        }
    } else if (cmd.rfind("video-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(10));
        if (scene && id.is_valid()) {
            scene->videoComponents[id] = VideoComponent{};
            std::cout << "[ControlApi] video-add " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("video-frame ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(12));
        std::string uuidStr, name;
        ss >> uuidStr;
        std::getline(ss, name);
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        const UUID id = UUID::from_string(uuidStr);
        if (scene && scene->videoComponents.contains(id) && !name.empty()) {
            scene->videoComponents[id].framePaths.push_back(name);
            std::cout << "[ControlApi] video-frame " << uuidStr << " '" << name << "'" << std::endl;
        }
    } else if (cmd.rfind("video-play ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(11));
        std::string uuidStr;
        int mode = 1;
        ss >> uuidStr >> mode;
        const UUID id = UUID::from_string(uuidStr);
        if (scene && scene->videoComponents.contains(id)) {
            scene->videoComponents[id].playing = mode != 0;
            std::cout << "[ControlApi] video-play " << uuidStr << " " << mode << std::endl;
        }
    } else if (cmd.rfind("gaussian-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(13));
        if (scene && id.is_valid()) {
            scene->gaussianSplatComponents[id] = GaussianSplatComponent{};
            std::cout << "[ControlApi] gaussian-add " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("gaussian-regen ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        const UUID id = UUID::from_string(cmd.substr(15));
        if (scene && scene->gaussianSplatComponents.contains(id)) {
            scene->gaussianSplatComponents[id].regenerate = true;
            std::cout << "[ControlApi] gaussian-regen " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("expression-add ", 0) == 0) {
        Scene* scene = m_editorScene.get();
        std::istringstream ss(cmd.substr(15));
        std::string uuidStr, headStr;
        ss >> uuidStr >> headStr;
        const UUID id = UUID::from_string(uuidStr);
        if (scene && id.is_valid()) {
            ExpressionComponent ex;
            ex.headEntity = UUID::from_string(headStr);
            scene->expressionComponents[id] = ex;
            std::cout << "[ControlApi] expression-add " << uuidStr << " head=" << headStr << std::endl;
        }
    } else if (cmd.rfind("asset-duplicate ", 0) == 0) {
        const UUID id = UUID::from_string(cmd.substr(16));
        const auto meta = m_assetRegistry.find(id);
        if (meta) {
            const std::filesystem::path dup = meta->sourcePath.parent_path() /
                (meta->sourcePath.stem().string() + "_copy" + meta->sourcePath.extension().string());
            AssetBrowserModel browser{ m_assetRegistry };
            const auto result = browser.duplicate_asset(id, dup);
            m_assetRegistry.save(std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db");
            std::cout << "[ControlApi] asset-duplicate -> " << (result ? "ok" : result.error) << std::endl;
        }
    } else if (cmd.rfind("asset-delete ", 0) == 0) {
        const UUID id = UUID::from_string(cmd.substr(13));
        AssetBrowserModel browser{ m_assetRegistry };
        const auto result = browser.delete_asset(id);
        m_assetRegistry.save(std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db");
        std::cout << "[ControlApi] asset-delete -> " << (result ? "ok" : result.error) << std::endl;
    } else if (cmd.rfind("reimport ", 0) == 0) {
        const UUID id = UUID::from_string(cmd.substr(9));
        const auto meta = m_assetRegistry.find(id);
        if (meta && m_assetPipeline) {
            const std::filesystem::path cookedRoot =
                std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "DerivedDataCache";
            const ImportResult result = m_assetPipeline->import({
                .source = meta->sourcePath, .cookedDirectory = cookedRoot,
                .importerVersion = meta->importerVersion, .settings = meta->importSettings });
            m_assetRegistry.save(std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "AssetRegistry.db");
            std::cout << "[ControlApi] reimport -> " << (result ? "ok" : result.error) << std::endl;
        }
    } else if (cmd.rfind("voxel-generate ", 0) == 0) {
        std::istringstream ss(cmd.substr(15));
        std::string uuidStr; uint32_t seed = 1337; float seaLevel = 24.0f;
        ss >> uuidStr; if (ss >> seed) {} if (ss >> seaLevel) {}
        const UUID id = UUID::from_string(uuidStr);
        m_voxelStructures.erase(id);
        ensure_voxel_volume(id, seed, seaLevel);
        m_voxelMeshesDirty.insert(id);
        std::cout << "[ControlApi] voxel-generate " << uuidStr << " seed=" << seed << std::endl;
    } else if (cmd.rfind("voxel-clear ", 0) == 0) {
        const UUID id = UUID::from_string(cmd.substr(12));
        const auto gridIt = m_voxelStructures.find(id);
        if (gridIt != m_voxelStructures.end()) {
            const auto& size = gridIt->second->size();
            for (int x = 0; x < size.x; ++x)
                for (int y = 0; y < size.y; ++y)
                    for (int z = 0; z < size.z; ++z)
                        gridIt->second->set(Engine::Voxel::Int3{ x, y, z }, Engine::Voxel::VoxelValue::air());
            m_voxelMeshesDirty.insert(id);
            std::cout << "[ControlApi] voxel-clear " << id.to_string() << std::endl;
        }
    } else if (cmd.rfind("voxel-paint ", 0) == 0) {
        std::istringstream ss(cmd.substr(12));
        std::string uuidStr; int x = 0, y = 0, z = 0, type = 1, mode = 0;
        ss >> uuidStr >> x >> y >> z >> type >> mode;
        const UUID id = UUID::from_string(uuidStr);
        const auto gridIt = m_voxelStructures.find(id);
        if (gridIt == m_voxelStructures.end()) {
            std::cout << "[ControlApi] voxel-paint: volume not generated yet" << std::endl;
        } else {
            if (mode == 1) gridIt->second->set(Engine::Voxel::Int3{ x, y, z }, Engine::Voxel::VoxelValue::air());
            else gridIt->second->set(Engine::Voxel::Int3{ x, y, z }, Engine::Voxel::VoxelValue{ static_cast<uint16_t>(type), 0, 255 });
            m_voxelMeshesDirty.insert(id);
            std::cout << "[ControlApi] voxel-paint " << uuidStr << " (" << x << "," << y << "," << z << ") type=" << type << " mode=" << mode << std::endl;
        }
    } else if (cmd.rfind("script-event ", 0) == 0) {
        const std::string ev = cmd.substr(13);
        if (m_playScript.start_event(ev)) std::cout << "[ControlApi] script-event '" << ev << "'" << std::endl;
        else std::cout << "[ControlApi] script-event: no such event handler" << std::endl;
    } else if (cmd == "script-pause") {
        m_scriptPauseRequested = true;
        std::cout << "[ControlApi] script paused" << std::endl;
    } else if (cmd == "script-continue") {
        m_scriptPauseRequested = false;
        if (m_playScript.status() == VMStatus::Paused) m_scriptDebugger.continue_run(10000, 0.0f);
        std::cout << "[ControlApi] script resumed" << std::endl;
    } else if (cmd == "script-step") {
        m_scriptPauseRequested = true;
        if (m_playScript.status() == VMStatus::Paused) m_scriptDebugger.step_into(0.0f);
        std::cout << "[ControlApi] script step" << std::endl;
    } else if (cmd.rfind("editor ", 0) == 0) {
        std::string tab = cmd.substr(7);
        if (tab == "render-graph" || tab == "render graph") tab = "Render Graph";
        else if (!tab.empty()) tab[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(tab[0])));
        m_specializedEditors.open_editor(tab);
        std::cout << "[ControlApi] editor tab '" << tab << "'" << std::endl;
    } else if (cmd.rfind("window ", 0) == 0) {
        const std::string w = cmd.substr(7);
        const auto toggle = [](bool& flag) { flag = !flag; };
        if (w == "viewport") toggle(m_showViewport);
        else if (w == "scene") toggle(m_showHierarchy);
        else if (w == "inspector") toggle(m_showInspector);
        else if (w == "assets") toggle(m_showContentBrowser);
        else if (w == "console") toggle(m_showConsole);
        else if (w == "dev") toggle(m_wickedTools.showDevWindow);
        else if (w == "guide") toggle(m_wickedTools.showGuideWindow);
        else if (w == "name") toggle(m_wickedTools.showNameWindow);
        else if (w == "layers") toggle(m_wickedTools.showLayerWindow);
        else if (w == "object") toggle(m_wickedTools.showObjectWindow);
        else if (w == "light") toggle(m_wickedTools.showLightWindow);
        else if (w == "camera") toggle(m_wickedTools.showCameraWindow);
        else if (w == "material") toggle(m_wickedTools.showMaterialWindow);
        else if (w == "sound") toggle(m_wickedTools.showSoundWindow);
        else if (w == "rigidbody") toggle(m_wickedTools.showRigidBodyWindow);
        else if (w == "collider") toggle(m_wickedTools.showColliderWindow);
        else if (w == "constraint") toggle(m_wickedTools.showConstraintWindow);
        else if (w == "softbody") toggle(m_wickedTools.showSoftBodyWindow);
        else if (w == "spring") toggle(m_wickedTools.showSpringWindow);
        else if (w == "decal") toggle(m_wickedTools.showDecalWindow);
        else if (w == "emitter") toggle(m_wickedTools.showEmitterWindow);
        else if (w == "hair") toggle(m_wickedTools.showHairParticleWindow);
        else if (w == "spline") toggle(m_wickedTools.showSplineWindow);
        else if (w == "forcefield") toggle(m_wickedTools.showForceFieldWindow);
        else if (w == "envprobe") toggle(m_wickedTools.showEnvProbeWindow);
        else if (w == "weather") toggle(m_wickedTools.showWeatherWindow);
        else if (w == "animation-tools") toggle(m_wickedTools.showAnimationWindow);
        else if (w == "armature") toggle(m_wickedTools.showArmatureWindow);
        else if (w == "humanoid") toggle(m_wickedTools.showHumanoidWindow);
        else if (w == "ik-tools") toggle(m_wickedTools.showIKWindow);
        else if (w == "expression") toggle(m_wickedTools.showExpressionWindow);
        else if (w == "terrain") toggle(m_wickedTools.showTerrainWindow);
        else if (w == "paint") toggle(m_wickedTools.showPaintToolWindow);
        else if (w == "mesh") toggle(m_wickedTools.showMeshWindow);
        else if (w == "importer") toggle(m_wickedTools.showModelImporterWindow);
        else if (w == "video") toggle(m_wickedTools.showVideoWindow);
        else if (w == "gaussian") toggle(m_wickedTools.showGaussianSplatWindow);
        else if (w == "theme") toggle(m_wickedTools.showThemeEditorWindow);
        else if (w == "project-creator") toggle(m_wickedTools.showProjectCreatorWindow);
        else if (w == "general") toggle(m_wickedTools.showGeneralWindow);
        else if (w == "graphics") toggle(m_wickedTools.showGraphicsWindow);
        else if (w == "profiler") toggle(m_wickedTools.showProfilerWindow);
        else { std::cout << "[ControlApi] window: unknown '" << w << "'" << std::endl; return; }
        std::cout << "[ControlApi] window toggled '" << w << "'" << std::endl;
    } else if (cmd.rfind("theme ", 0) == 0) {
        float r = 0.1f, g = 0.11f, b = 0.14f, pr = 0.2f, pg = 0.2f, pb = 0.2f;
        std::istringstream ss(cmd.substr(6));
        ss >> r >> g >> b >> pr >> pg >> pb;
        m_wickedTools.set_theme(glm::vec3(r, g, b), glm::vec3(pr, pg, pb));
        ImGuiStyle& style = ImGui::GetStyle();
        style.Colors[ImGuiCol_WindowBg] = ImVec4(r, g, b, 1.0f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(pr, pg, pb, 1.0f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(pr, pg, pb, 1.0f);
        style.Colors[ImGuiCol_MenuBarBg] = ImVec4(pr, pg, pb, 1.0f);
        const float lift = 0.08f;
        style.Colors[ImGuiCol_FrameBg] = ImVec4(pr + lift, pg + lift, pb + lift, 1.0f);
        style.Colors[ImGuiCol_Button] = ImVec4(pr + lift, pg + lift, pb + lift, 1.0f);
        std::cout << "[ControlApi] theme applied" << std::endl;
    } else if (cmd.rfind("weather ", 0) == 0) {
        float sunR = 1.0f, sunG = 0.9f, sunB = 0.7f, fogDensity = 0.0f, fogStart = 0.0f, skyExposure = 1.0f, rain = 0.0f;
        std::istringstream ss(cmd.substr(8));
        ss >> sunR >> sunG >> sunB >> fogDensity >> fogStart >> skyExposure >> rain;
        if (m_editorScene) {
            UUID weatherId{ 0, 0 };
            for (const auto& [id, entity] : m_editorScene->get_entities()) {
                (void)entity;
                if (m_editorScene->weatherComponents.contains(id)) { weatherId = id; break; }
            }
            if (!weatherId.is_valid()) {
                Entity w = m_editorScene->create_entity("Weather");
                weatherId = w.get_id();
                m_editorScene->weatherComponents[weatherId] = WeatherComponent{};
            }
            auto& w = m_editorScene->weatherComponents[weatherId];
            w.sunColor = glm::vec3(sunR, sunG, sunB);
            w.fogDensity = fogDensity; w.fogStart = fogStart; w.skyExposure = skyExposure; w.rainAmount = rain;
            std::cout << "[ControlApi] weather applied" << std::endl;
        }
    } else if (cmd.rfind("selftest ", 0) == 0) {
        // Accept both numeric indices (0-4) and the friendly names
        // (rendergraph/hdr/material/play/build) so a typo like "material"
        // never crashes the editor with a std::stoi exception.
        std::string arg = cmd.substr(9);
        int which = -1;
        try {
            which = std::stoi(arg);
        } catch (...) {
            static const char* kTestNames[] = { "rendergraph", "hdr", "material", "play", "build" };
            for (int i = 0; i < 5; ++i) {
                if (arg == kTestNames[i]) { which = i; break; }
            }
        }
        if (which < 0 || which >= 5) {
            std::cout << "[ControlApi] selftest: invalid test '" << arg << "'" << std::endl;
        } else {
            m_lastSelfTestResult = run_editor_self_test(which);
            std::cout << "[ControlApi] selftest " << arg << " -> " << m_lastSelfTestResult << std::endl;
        }
    } else if (cmd == "package") {
        const std::string result = package_assets_only();
        std::cout << "[ControlApi] package -> " << result << std::endl;
    } else if (cmd == "hot-reload") {
        if (m_assetHotReload) m_assetHotReload->watch_registered_assets();
        const auto reloaded = m_assetHotReload ? m_assetHotReload->poll() : std::vector<AssetMetadata>{};
        std::cout << "[ControlApi] hot-reload -> " << reloaded.size() << " asset(s) reimported" << std::endl;
    } else {
        std::cout << "[ControlApi] ignored '" << cmd << "' (state="
                  << static_cast<int>(m_playMode.get_state()) << ")" << std::endl;
    }
}

std::string EditorApplication::run_editor_self_test(int which) {
    static const char* kTestEnv[] = {
        "VC_EDITOR_TEST_RENDERGRAPH",
        "VC_EDITOR_TEST_HDR",
        "VC_EDITOR_TEST_MATERIAL",
        "VC_EDITOR_TEST_PLAY",
        "VC_EDITOR_TEST_BUILD",
    };
    if (which < 0 || which >= 5) return "Erro: teste inválido";
#ifdef _WIN32
    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0) {
        return "Erro: não foi possível localizar o executável";
    }
    // The child inherits the environment at creation time; set the test flag
    // only for the duration of the spawn (the parent never re-reads it after
    // startup). CREATE_NO_WINDOW keeps the headless run out of the user's way.
    SetEnvironmentVariableA(kTestEnv[which], "1");
    STARTUPINFOA si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    std::string cmdLine = std::string("\"") + exePath + "\"";
    if (!CreateProcessA(exePath, cmdLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        SetEnvironmentVariableA(kTestEnv[which], nullptr);
        return std::string("Erro: falha ao iniciar o teste (code ") +
               std::to_string(GetLastError()) + ")";
    }
    // Never wait forever: a hung headless child would wedge the editor's main
    // loop (and with it the Control API). 120s is generous for any build test.
    const DWORD waitMs = 120000;
    const DWORD waitResult = WaitForSingleObject(pi.hProcess, waitMs);
    DWORD code = 0;
    if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &code);
    } else {
        TerminateProcess(pi.hProcess, 1);
        code = 1;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    SetEnvironmentVariableA(kTestEnv[which], nullptr);
    return code == 0 ? "PASS" : ("FAIL (exit " + std::to_string(code) + ")");
#else
    // Non-Windows fallback: spawn via shell and read the exit status.
    const std::string cmd =
        std::string(kTestEnv[which]) + "=1 ./VulkanEngineEditor >/dev/null 2>&1";
    const int rc = std::system(cmd.c_str());
    return rc == 0 ? "PASS" : ("FAIL (exit " + std::to_string(rc) + ")");
#endif
}

std::string EditorApplication::package_assets_only() {
    std::vector<UUID> roots;
    for (const AssetMetadata& asset : m_assetRegistry.snapshot()) {
        if (asset.isCooked) roots.push_back(asset.id);
    }
    if (roots.empty()) {
        return tr("Erro: nenhum asset cozido para empacotar (importe assets primeiro).",
                  "Error: no cooked assets to package (import assets first).");
    }
    const std::filesystem::path out =
        std::filesystem::path(VULKANCRAFT_SOURCE_DIR) / "Intermediate" / "Package";
    const AssetPackageResult packaged = AssetPackager::package(m_assetRegistry, roots, out);
    if (!packaged) {
        return std::string(tr("Erro: ", "Error: ")) + packaged.error;
    }
    std::cout << "[Editor] standalone package: " << packaged.assets.size()
              << " asset(s) -> " << out.string() << std::endl;
    return tr("OK: ", "OK: ") + std::to_string(packaged.assets.size()) +
           tr(" asset(s) empacotados em ", " asset(s) packaged to ") + out.string();
}

void EditorApplication::update_editor_camera(float deltaTime) {
    // Respond to the mouse over the rendered image, not to ImGui window focus:
    // focus can go stale (another panel taking it), which made the viewport
    // appear to stop answering the mouse entirely.
    if (!m_viewportImageHovered) return;

    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);
    const glm::vec2 mouse(static_cast<float>(mx), static_cast<float>(my));
    const glm::vec2 mouseDelta = mouse - m_lastMousePos;
    m_lastMousePos = mouse;

    EditorCamera& cam = m_editorCamera;
    const glm::vec3 front = cam.get_front();
    const glm::vec3 right = cam.get_right();
    const glm::vec3 up = cam.get_up();

    // Don't let camera keys fight the user typing in Inspector/text fields.
    // NOTE: io.WantCaptureKeyboard is true whenever the mouse hovers ANY
    // window, which would kill WASD the moment the cursor is over the 3D view;
    // io.WantTextInput is true only while an actual text field is being typed.
    ImGuiIO& io = ImGui::GetIO();
    const bool keysFree = !io.WantTextInput;

    // Orbit (right drag) / pan (middle drag).
    const bool orbitHeld = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    const bool panHeld = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    if (orbitHeld && !m_gizmoDragging) {
        cam.yaw += mouseDelta.x * cam.sensitivity;
        cam.pitch = glm::clamp(cam.pitch - mouseDelta.y * cam.sensitivity, -89.0f, 89.0f);
    }
    if (panHeld) {
        const float panScale = cam.orbitDistance * 0.0016f;
        cam.orbitTarget += (-right * mouseDelta.x + up * mouseDelta.y) * panScale;
    }

    // Fly (WASD): free-fly whenever the mouse is over the viewport and the
    // keyboard is not captured by a text field — no right-button required.
    if (keysFree) {
        const float speed = cam.speed * (glfwGetKey(m_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 4.0f : 1.0f);
        glm::vec3 move(0.0f);
        if (glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS) move += front;
        if (glfwGetKey(m_window, GLFW_KEY_S) == GLFW_PRESS) move -= front;
        if (glfwGetKey(m_window, GLFW_KEY_D) == GLFW_PRESS) move += right;
        if (glfwGetKey(m_window, GLFW_KEY_A) == GLFW_PRESS) move -= right;
        if (glfwGetKey(m_window, GLFW_KEY_E) == GLFW_PRESS) move += up;
        if (glfwGetKey(m_window, GLFW_KEY_Q) == GLFW_PRESS) move -= up;
        if (glm::length(move) > 0.0f) {
            cam.orbitTarget += glm::normalize(move) * speed * deltaTime;
        }
    }

    // Scroll zoom: the wheel inside the 3D view ALWAYS dollies toward/away
    // from the orbit focus — it never scrolls any panel (the viewport is
    // NoScrollbar|NoScrollWithMouse, and the delta is consumed here). The
    // delta comes from our own GLFW callback accumulator, not io.MouseWheel,
    // which ImGui zeroes at the end of NewFrame before we can read it. When
    // the viewport is NOT hovered the accumulator is dropped so ImGui keeps
    // scrolling other panels normally.
    if (m_viewportHovered || m_viewportImageHovered) {
        if (m_scrollAccum != 0.0) {
            cam.orbitDistance = glm::clamp(
                cam.orbitDistance * (1.0f - static_cast<float>(m_scrollAccum) * 0.1f), 0.5f, 5000.0f);
            m_scrollAccum = 0.0;
        }
    } else {
        m_scrollAccum = 0.0;
    }

    recompute_editor_camera_position();
}

void EditorApplication::recompute_editor_camera_position() {
    // Recompute the camera position from target + spherical offset.
    m_editorCamera.position = m_editorCamera.orbitTarget -
                              euler_direction(m_editorCamera.yaw, m_editorCamera.pitch) *
                              m_editorCamera.orbitDistance;
}

void EditorApplication::process_viewport_input() {
    // Gizmo keys work on hover (mouse over the 3D image), not on ImGui window
    // focus — focus can sit on another panel and would freeze the keys.
    if (!m_viewportImageHovered) return;
    ImGuiIO& io = ImGui::GetIO();
    // Gizmo mode switching: Q / W / E / R
    if (!io.WantCaptureKeyboard) {
        if (glfwGetKey(m_window, GLFW_KEY_Q) == GLFW_PRESS && m_gizmoMode != GizmoMode::Select) {
            m_gizmoMode = GizmoMode::Select;
        }
        if (glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS && m_gizmoMode != GizmoMode::Translate) {
            m_gizmoMode = GizmoMode::Translate;
        }
        if (glfwGetKey(m_window, GLFW_KEY_E) == GLFW_PRESS && m_gizmoMode != GizmoMode::Rotate) {
            m_gizmoMode = GizmoMode::Rotate;
        }
        if (glfwGetKey(m_window, GLFW_KEY_R) == GLFW_PRESS && m_gizmoMode != GizmoMode::Scale) {
            m_gizmoMode = GizmoMode::Scale;
        }
    }
}

glm::vec3 EditorApplication::unproject_to_plane(glm::vec2 mouseScreen, const glm::vec3& planePoint,
                                                const glm::vec3& planeNormal, const glm::mat4& invViewProj) const {
    const float ndcX = (mouseScreen.x - m_viewportImagePos.x) / std::max(1.0f, m_viewportImageSize.x) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (mouseScreen.y - m_viewportImagePos.y) / std::max(1.0f, m_viewportImageSize.y) * 2.0f;
    const glm::vec4 near4 = invViewProj * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    const glm::vec4 far4 = invViewProj * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    const glm::vec3 nearP = glm::vec3(near4) / near4.w;
    const glm::vec3 farP = glm::vec3(far4) / far4.w;
    const glm::vec3 dir = glm::normalize(farP - nearP);
    const float denom = glm::dot(dir, planeNormal);
    if (std::abs(denom) < 1e-6f) return planePoint;
    const float t = glm::dot(planePoint - nearP, planeNormal) / denom;
    return nearP + dir * t;
}

bool EditorApplication::gizmo_axis_hit_test(glm::vec2 mouseScreen) {
    m_hoveredAxis = GizmoAxis::None;
    if (!m_editorScene || !m_selectedEntity.is_valid()) return false;
    const auto it = m_editorScene->transformComponents.find(m_selectedEntity.get_id());
    if (it == m_editorScene->transformComponents.end()) return false;
    const glm::vec3 origin = it->second.position;
    // World/Local hit test: axes rotate with the entity in local mode.
    const glm::quat gizmoRotation = m_gizmoLocal
        ? glm::quat(glm::radians(it->second.rotation))
        : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    const auto axisWorld = [&](int axis) -> glm::vec3 { return gizmoRotation * kAxisDirs[axis]; };

    const float aspect = m_viewportImageSize.x / std::max(1.0f, m_viewportImageSize.y);
    const glm::mat4 viewProj = m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix();
    const auto project = [&](const glm::vec3& world) -> glm::vec2 {
        glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
        if (std::abs(clip.w) < 1e-6f) return { -1e9f, -1e9f };
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        return glm::vec2(m_viewportImagePos.x + (ndc.x * 0.5f + 0.5f) * m_viewportImageSize.x,
                         m_viewportImagePos.y + (-ndc.y * 0.5f + 0.5f) * m_viewportImageSize.y);
    };

    const glm::vec2 originScreen = project(origin);
    float bestDist = 1e18f;
    GizmoAxis best = GizmoAxis::None;
    const float gizmoLen = (m_gizmoMode == GizmoMode::Rotate) ? 1.45f : 1.55f;
    for (int axis = 0; axis < 3; ++axis) {
        float dist = 1e18f;
        if (m_gizmoMode == GizmoMode::Rotate) {
            // Distance to the projected ring polyline.
            for (int s = 0; s < 48; ++s) {
                const float a0 = glm::two_pi<float>() * static_cast<float>(s) / 48.0f;
                const float a1 = glm::two_pi<float>() * static_cast<float>(s + 1) / 48.0f;
                const glm::vec3 dir = axisWorld(axis);
                glm::vec3 u = glm::normalize(glm::cross(dir, std::abs(dir.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
                glm::vec3 v = glm::normalize(glm::cross(dir, u));
                const glm::vec3 p0 = origin + u * (std::cos(a0) * gizmoLen) + v * (std::sin(a0) * gizmoLen);
                const glm::vec3 p1 = origin + u * (std::cos(a1) * gizmoLen) + v * (std::sin(a1) * gizmoLen);
                dist = std::min(dist, dist_point_segment(mouseScreen, project(p0), project(p1)));
            }
        } else {
            const glm::vec2 tipScreen = project(origin + axisWorld(axis) * gizmoLen);
            dist = dist_point_segment(mouseScreen, originScreen, tipScreen);
        }
        if (dist < 14.0f && dist < bestDist) {
            bestDist = dist;
            best = static_cast<GizmoAxis>(axis + 1);
        }
    }
    m_hoveredAxis = best;
    return best != GizmoAxis::None;
}

void EditorApplication::start_gizmo_drag(glm::vec2 mouseScreen) {
    if (!m_editorScene || !m_selectedEntity.is_valid()) return;
    const UUID id = m_selectedEntity.get_id();
    if (!m_editorScene->transformComponents.contains(id)) return;
    const TransformComponent& t = m_editorScene->transformComponents.at(id);

    m_gizmoDragging = true;
    m_gizmoDragEntityStart = t.position;
    m_gizmoDragRotStart = t.rotation;
    m_gizmoDragScaleStart = t.scale;
    // World/Local: in local mode the drag axis follows the entity rotation.
    if (m_gizmoLocal) {
        m_gizmoAxisWorld = glm::quat(glm::radians(t.rotation)) * kAxisDirs[static_cast<int>(m_activeAxis) - 1];
    } else {
        m_gizmoAxisWorld = kAxisDirs[static_cast<int>(m_activeAxis) - 1];
    }
    m_gizmoDragPlaneNormal = glm::normalize(m_editorCamera.orbitTarget - m_editorCamera.position);
    if (glm::length(m_gizmoDragPlaneNormal) < 1e-5f) m_gizmoDragPlaneNormal = glm::vec3(0, 0, 1);

    const float aspect = m_viewportImageSize.x / std::max(1.0f, m_viewportImageSize.y);
    const glm::mat4 invViewProj = glm::inverse(
        m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix());
    m_gizmoDragPlanePoint = unproject_to_plane(mouseScreen, t.position, m_gizmoDragPlaneNormal, invViewProj);

    if (m_gizmoMode == GizmoMode::Rotate) {
        glm::vec3 toPoint = m_gizmoDragPlanePoint - t.position;
        if (glm::length(toPoint) < 1e-5f) toPoint = m_gizmoAxisWorld == glm::vec3(0, 1, 0) ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        glm::vec3 ref = toPoint - m_gizmoAxisWorld * glm::dot(toPoint, m_gizmoAxisWorld);
        if (glm::dot(ref, ref) < 1e-6f) ref = glm::normalize(glm::cross(m_gizmoAxisWorld, glm::vec3(0, 0, 1)));
        m_gizmoDragAngleRef = glm::normalize(ref);
    }
}

void EditorApplication::update_gizmo_drag(glm::vec2 mouseScreen) {
    if (!m_editorScene || !m_selectedEntity.is_valid() || !m_gizmoDragging) return;
    const UUID id = m_selectedEntity.get_id();
    auto it = m_editorScene->transformComponents.find(id);
    if (it == m_editorScene->transformComponents.end()) return;

    const float aspect = m_viewportImageSize.x / std::max(1.0f, m_viewportImageSize.y);
    const glm::mat4 invViewProj = glm::inverse(
        m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix());
    const glm::vec3 planePoint = unproject_to_plane(mouseScreen, m_gizmoDragPlanePoint,
                                                    m_gizmoDragPlaneNormal, invViewProj);
    const bool snap = ImGui::GetIO().KeyCtrl;
    const int axisIndex = static_cast<int>(m_activeAxis) - 1;

    if (m_gizmoMode == GizmoMode::Translate) {
        float delta = glm::dot(planePoint - m_gizmoDragPlanePoint, m_gizmoAxisWorld);
        if (snap) delta = std::round(delta / m_snapTranslate) * m_snapTranslate;
        const glm::vec3 newPos = m_gizmoDragEntityStart + m_gizmoAxisWorld * delta;
        m_undo.execute_or_merge_property(
            "Move Entity",
            [this, id, newPos] { m_editorScene->transformComponents.at(id).position = newPos; },
            [this, id, start = m_gizmoDragEntityStart] {
                m_editorScene->transformComponents.at(id).position = start;
            });
    } else if (m_gizmoMode == GizmoMode::Rotate) {
        glm::vec3 toPoint = planePoint - m_gizmoDragEntityStart;
        if (glm::length(toPoint) < 1e-5f) return;
        glm::vec3 v = toPoint - m_gizmoAxisWorld * glm::dot(toPoint, m_gizmoAxisWorld);
        if (glm::dot(v, v) < 1e-6f) return;
        v = glm::normalize(v);
        const float angle = glm::degrees(std::atan2(
            glm::dot(glm::cross(m_gizmoDragAngleRef, v), m_gizmoAxisWorld),
            glm::dot(m_gizmoDragAngleRef, v)));
        const float snapped = snap ? std::round(angle / m_snapRotate) * m_snapRotate : angle;
        glm::vec3 newRot = m_gizmoDragRotStart;
        newRot[axisIndex] += snapped;
        m_undo.execute_or_merge_property(
            "Rotate Entity",
            [this, id, newRot] { m_editorScene->transformComponents.at(id).rotation = newRot; },
            [this, id, start = m_gizmoDragRotStart] {
                m_editorScene->transformComponents.at(id).rotation = start;
            });
    } else if (m_gizmoMode == GizmoMode::Scale) {
        float delta = glm::dot(planePoint - m_gizmoDragPlanePoint, m_gizmoAxisWorld);
        float factor = 1.0f + delta / 1.0f;
        if (snap) factor = std::round(factor / m_snapScale) * m_snapScale;
        factor = std::max(factor, 0.02f);
        glm::vec3 newScale = m_gizmoDragScaleStart;
        newScale[axisIndex] = m_gizmoDragScaleStart[axisIndex] * factor;
        m_undo.execute_or_merge_property(
            "Scale Entity",
            [this, id, newScale] { m_editorScene->transformComponents.at(id).scale = newScale; },
            [this, id, start = m_gizmoDragScaleStart] {
                m_editorScene->transformComponents.at(id).scale = start;
            });
    }
}

// ===========================================================================
// Cooked mesh resources (real imported geometry in the viewport)
// ===========================================================================


} // namespace Engine
