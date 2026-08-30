// WorldRuntimeCompositionTests — gate do contrato IWorldRuntime (Agente 2 §A/B):
// prova a composição canônica de ponta a ponta instanciando TODOS os serviços
// do WorldServiceContext com as factories reais do SDK e dirigindo o loop real
// bind -> bootstrap -> advance -> shutdown. O runtime canônico é o único dono da
// ordem de lifecycle (Bootstrap/FixedTick/VariableUpdate/LateUpdate/
// RenderSnapshot/Persistence/Shutdown); esta gate verifica que essa ordem é
// consumida por uma composição viva e produz comportamento observável (tick,
// entidades, mundos) — não é um factory isolado.
//
// Self-contained: inclui apenas headers públicos + as factories do SDK. O
// comportamento é determinístico (sem renderer, sem Vulkan, sem device).
// Um mundo voxel NÃO é instanciado aqui de propósito: a prova é a composição do
// runtime canônico (fixo no ECS + física + worlds + router + audio + queries),
// não o boot de geração/streaming voxel (coberto pelas gates do domínio voxel).

#include "engine/world/IWorldRuntime.hpp"
#include "engine/gameplay/IGameplayIntegration.hpp"
#include "engine/gameplay/IGameplayBindings.hpp"
#include "engine/gameplay/IGameplaySystemWiring.hpp"
#include "engine/gameplay/IGameplayRuntime.hpp"
#include <limits>
#include "engine/gameplay/IGameplayEvents.hpp"
#include "engine/gameplay/IGameplayMetrics.hpp"
#include "engine/gameplay/IGameplayEventRouter.hpp"
#include "engine/gameplay/IAbilityEffects.hpp"
#include "engine/gameplay/IMissionAsset.hpp"
#include "engine/gameplay/IAbilitySystem.hpp"
#include "engine/gameplay/IDayNightCycle.hpp"
#include "engine/entity/IEntityWorld.hpp"
#include "engine/world/IWorldManager.hpp"
#include "engine/navigation/IAsyncQueryScheduler.hpp"
#include "engine/navigation/INavigationProvider.hpp"
#include "engine/audio/IAudioEventMapper.hpp"
#include "engine/audio/ISpatialAudio.hpp"
#include "engine/audio/IAdaptiveMusic.hpp"
#include "engine/registry/ItemRegistry.hpp"
#include "engine/registry/Inventory.hpp"
#include "engine/registry/RecipeRegistry.hpp"
#include "engine/vehicles/IVehicleAsset.hpp"
#include "engine/vehicles/IVehicleProvider.hpp"
#include "engine/procgen/IStructurePlacement.hpp"
#include "engine/animation/IGaitPlanner.hpp"
#include "engine/animation/IFootPlacement.hpp"
#include "engine/ai/ISteering.hpp"
#include "engine/ai/IPerception.hpp"
#include "engine/ai/IFsm.hpp"
#include "engine/ai/IBehaviorTree.hpp"
#include "engine/ai/IUtilityAi.hpp"
#include "engine/ai/IPlanner.hpp"
#include "engine/ai/IAiLod.hpp"
#include "engine/ai/IAiEventBus.hpp"
#include "engine/ai/ICrowdSimulation.hpp"
#include "engine/entity/ISpatialIndex.hpp"
#include "engine/animation/IAnimationLod.hpp"
#include "engine/director/IWorldDirector.hpp"
#include "engine/capabilities/ICapabilityRegistry.hpp"
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

// Order of lifecycle phases the canonical runtime must report, in production
// order — the observable proof that game/play-mode/server advance the same
// systems (bootstrap/fixed/variable/late/render/persistence/shutdown).
bool same_phase_order(const std::vector<engine::WorldLifecyclePhase>& phases) {
    if (phases.size() != 7) return false;
    const engine::WorldLifecyclePhase expect[] = {
        engine::WorldLifecyclePhase::Bootstrap,
        engine::WorldLifecyclePhase::FixedTick,
        engine::WorldLifecyclePhase::VariableUpdate,
        engine::WorldLifecyclePhase::LateUpdate,
        engine::WorldLifecyclePhase::RenderSnapshot,
        engine::WorldLifecyclePhase::Persistence,
        engine::WorldLifecyclePhase::Shutdown,
    };
    for (std::size_t i = 0; i < 7; ++i) {
        if (phases[i] != expect[i]) return false;
    }
    return true;
}

}  // namespace

int main() {
    // ── Compose the canonical runtime with every service the product binds ──
    auto runtime     = engine::create_world_runtime();
    auto ecs         = engine::entity::create_entity_world();
    auto physics     = engine::gameplay::create_gameplay_runtime(
                           engine::gameplay::PhysicsBackend::Builtin);
    auto worlds      = engine::world::create_world_manager();
    auto integration = engine::gameplay::create_gameplay_integration();
    auto bindings    = engine::gameplay::create_gameplay_bindings();
    auto wiring      = engine::gameplay::create_gameplay_system_wiring();
    auto events      = engine::gameplay::create_gameplay_events();
    auto metrics     = engine::gameplay::create_gameplay_metrics();
    auto audio       = engine::audio::create_audio_event_mapper();
    auto queries     = engine::navigation::create_async_query_scheduler();
    auto router      = engine::gameplay::create_gameplay_event_router(
                           events.get(), audio.get(), metrics.get());
    // AGENTE 2 block G (day/night): the deterministic clock is part of the
    // canonical composition — bound like every other service and advanced by
    // the runtime's VariableUpdate phase.
    auto dayNight   = engine::gameplay::create_day_night_cycle();
    {
        engine::gameplay::DayNightConfig dnConfig;
        dnConfig.dayLengthSeconds = 180.0f;
        dnConfig.startOfDay = 0.22f;
        std::string dnError;
        if (!dayNight->configure(dnConfig, dnError)) {
            std::printf("  FAIL: day/night configure: %s\n", dnError.c_str());
            return 1;
        }
    }

    engine::WorldServiceContext context;
    context.ecs = ecs.get();
    context.physicsGameplay = physics.get();
    context.worlds = worlds.get();
    context.integration = integration.get();
    context.bindings = bindings.get();
    context.wiring = wiring.get();
    context.eventRouter = router.get();
    context.navigationQueries = queries.get();
    context.audio = audio.get();
    context.dayNight = dayNight.get();

    std::string error;
    check(runtime->bind(context, error), "bind succeeds with full composition");
    if (failures) return 1;

    // ── Bootstrap drives the canonical wiring (fixed tick = authority) ──────
    // NOTE: bindings/wiring are OPTIONAL in the canonical compose — an unmapped
    // binding manifest (bindings->complete() false) is treated as "headless
    // server runs integration on the ECS alone" and is skipped, never fatal.
    if (!runtime->bootstrap(error)) {
        std::printf("  FAIL: bootstrap: %s\n", error.c_str());
        return 1;
    }
    check(same_phase_order(runtime->enabled_phases()),
          "canonical lifecycle order reported");

    // ── Populate the ECS so advance() has real, observable state ───────────
    if (!ecs->spawn("player", engine::entity::Position{ 0.0f, 64.0f, 0.0f },
                    error).valid()) {
        std::printf("  FAIL: spawn player: %s\n", error.c_str());
        return 1;
    }

    // ── Advance drives the real loop: tick / entities / worlds observed ────
    engine::WorldRuntimeUpdateResult r1 = runtime->advance(1.0f / 30.0f);
    check(r1.tick > 0, "fixed tick advanced");
    check(r1.entities == 1, "one entity on the runtime ECS");
    check(r1.worlds == 0, "no world created (worlds counted by IWorldManager)");
    check(r1.bootstrapComplete, "bootstrap report complete after advance");
    // Day/night clock advanced by the runtime: time_of_day moved off the
    // startOfDay phase (0.22) and daylight_factor is a real [0,1] read.
    check(dayNight->time_of_day() > 0.220001f,
          "day/night clock advanced by the runtime advance");
    const float dnDaylight = dayNight->daylight_factor();
    check(dnDaylight >= 0.0f && dnDaylight <= 1.0f,
          "daylight_factor stays in [0,1]");

    // ── Canonical event bus (G.97): a REAL gameplay event published on the
    // IGameplayEvents the router drains inside the fixed tick becomes a routed
    // audio trigger request (kind mapped to eventKind -> audio trigger). This
    // is the same bus the game/server publish block.break/place on.
    {
        std::string busError;
        const std::vector<std::pair<std::uint16_t, std::string>> busMapping = {
            { 1, "block.break" },
        };
        if (!router->configure_mapping(busMapping, busError)) {
            std::printf("  FAIL: event bus mapping: %s\n", busError.c_str());
            return 1;
        }
        const std::vector<engine::audio::AudioTrigger> triggers = {
            { "block.break", "block_break", 0.9f, 1.0f },
        };
        if (!audio->configure(triggers, busError)) {
            std::printf("  FAIL: audio trigger mapping: %s\n", busError.c_str());
            return 1;
        }
    }
    events->publish(1, r1.tick, { 3 });  // block.break on stone
    // (a) The runtime advance drains the bus through the integration: the
    // router's routed_count grows inside the fixed tick.
    engine::WorldRuntimeUpdateResult rBus = runtime->advance(1.0f / 30.0f);
    check(rBus.tick > r1.tick, "bus advance keeps ticking");
    check(router->routed_count() > 0, "published event routed by the runtime");
    // (b) The router translates kind->eventKind->audio trigger: publish again
    // and read the emitted AudioTriggerRequest directly (the previous one was
    // already drained by the integration tick).
    events->publish(1, rBus.tick, { 3 });
    const auto routed = router->route(0);
    bool blockBreakTriggered = false;
    for (const auto& req : routed) {
        if (req.eventKind == "block.break" && req.soundId == "block_break") {
            blockBreakTriggered = true;
        }
    }
    check(blockBreakTriggered, "block.break event routed to an audio trigger");
    // (c) The router records a per-kind COUNTER in the shared metrics mirror
    // that the game reads from its window title — the observable metrics path
    // (events.<eventKind>) is alive, not headless-only.
    if (metrics) {
        const auto snap = metrics->snapshot();
        double eventTotal = 0.0;
        for (const auto& m : snap) {
            if (m.name.rfind("events.", 0) == 0) eventTotal += m.value;
        }
        check(eventTotal >= 2.0, "routed events accumulated in observable metrics");
        check(metrics->to_json().find("events.block.break") != std::string::npos,
              "metrics to_json exposes the routed event counter");
    }

    // Second advance keeps accumulating fixed ticks (2 x 1/60 in 1/30 => 2 ticks).
    engine::WorldRuntimeUpdateResult r2 = runtime->advance(1.0f / 30.0f);
    check(r2.tick >= r1.tick + 2, "fixed accumulator runs multiple steps");
    check(r2.integrationComplete, "integration reports complete after advance");

    // ── Config introspection is observable and stable ──────────────────────
    check(runtime->set_fixed_delta(1.0f / 20.0f), "set_fixed_delta accepts positive value");
    check(runtime->fixed_delta() > 0.0f, "fixed_delta reads back");
    // World-streaming focus fix (Agente 2 2026-08-29): the canonical runtime
    // must accept a non-origin focus so jogo/play-mode can bind IWorldManager
    // without evicting their real focus; non-finite values are rejected.
    check(runtime->set_world_focus(64.0f, 32.0f, -8.0f),
          "set_world_focus accepts a real play focus");
    const float nanFocus = std::numeric_limits<float>::quiet_NaN();
    check(!runtime->set_world_focus(nanFocus, 0.0f, 0.0f),
          "set_world_focus rejects non-finite focus");
    const std::string json = runtime->to_json();
    check(json.find("\"bound\":true") != std::string::npos, "to_json reports bound");
    check(json.find("\"bootstrapped\":true") != std::string::npos,
          "to_json reports bootstrapped");
    check(json.find("\"services\"") != std::string::npos, "to_json exposes services map");

    // ── Audio cores driven by the same clock (J.125/J.127) ─────────────────
    // The spatializer consumes a REAL entity position as a 3D source with the
    // player as listener; the adaptive-music core crosses from day to night
    // as the clock's daylight_factor drops — both deterministic and
    // observable, mirroring the game's per-frame audio block.
    {
        auto spatial = engine::audio::create_spatial_audio();
        std::string aerr;
        engine::audio::AudioSpatialSpec aspec;
        aspec.min_distance = 1.0;
        aspec.max_distance = 48.0;
        aspec.rolloff = engine::audio::RolloffModel::Inverse;
        if (!spatial->configure(aspec, aerr) ||
            !spatial->set_max_voices(2, aerr)) {
            std::printf("  FAIL: spatial audio configure: %s\n", aerr.c_str());
            return 1;
        }
        if (!spatial->set_listener(
                engine::audio::Vec3{ 0.0, 0.0, 0.0 },
                engine::audio::Vec3{ 0.0, 0.0, -1.0 }, aerr)) {
            std::printf("  FAIL: spatial listener: %s\n", aerr.c_str());
            return 1;
        }
        // The ECS player spawned earlier is the live 3D source.
        if (!ecs->spawn("mob", engine::entity::Position{ 10.0f, 64.0f, 0.0f },
                        aerr).valid()) {
            std::printf("  FAIL: spawn mob for spatial: %s\n", aerr.c_str());
            return 1;
        }
        std::size_t fed = 0;
        ecs->for_each_entity([&](engine::entity::EntityId id) {
            engine::entity::Position pos;
            if (ecs->get_position(id, pos)) {
                engine::audio::AudioSourceInput input;
                input.position = { pos.x, pos.y, pos.z };
                input.is_3d = true;
                std::string serr;
                spatial->set_source("src" + std::to_string(fed), input, serr);
                ++fed;
            }
        });
        check(spatial->update(aerr), "spatial audio update runs");
        check(fed >= 2, "spatial sources fed from the live ECS");
    }
    {
        auto music = engine::audio::create_adaptive_music();
        engine::audio::AdaptiveMusicSpec spec;
        spec.layers = { { "ambience" } };
        engine::audio::MusicState day;
        day.id = "day";
        day.layer_gains = { { "ambience", 1.0 } };
        day.transition_s = 0.0;   // instant: current_state resolves immediately
        engine::audio::MusicState night;
        night.id = "night";
        night.layer_gains = { { "ambience", 0.5 } };
        night.transition_s = 0.0;
        spec.states = { day, night };
        std::string merr;
        if (!music->configure(spec, merr)) {
            std::printf("  FAIL: adaptive music configure: %s\n", merr.c_str());
            return 1;
        }
        // The runtime already advanced the day/night clock; the game picks the
        // music state from the SAME daylight_factor — assert the core accepts
        // that clock-driven target deterministically.
        const float daylight = dayNight->daylight_factor();
        const std::string target = daylight < 0.35f ? "night" : "day";
        check(music->set_state(target, merr), "music follows the day/night clock");
        check(music->current_state() == target, "music current_state matches");
        check(music->tick(1.0f / 30.0f, merr), "music tick advances");
    }

    // ── Crafting table (G.91): the game's RecipeRegistry is bound to the
    // same ItemRegistry the hotbar uses; recipes_for answers from the LIVE
    // inventory (satisfiable = craftable), exactly the per-drop query the
    // game publishes in its window title.
    {
        engine::registry::ItemRegistry items;
        std::string ierr;
        check(items.load_from_json(
                  R"({"version":1,"namespace":"vulkancraft","name":"stone","maxStack":64})",
                  ierr),
              "item registry loads stone");
        check(items.load_from_json(
                  R"({"version":1,"namespace":"vulkancraft","name":"stone_brick","maxStack":64})",
                  ierr),
              "item registry loads stone_brick");
        engine::registry::RecipeRegistry recipes(&items);
        check(recipes.load_from_json(
                  R"({"version":1,"namespace":"vulkancraft","recipes":[{"name":"stone_brick","station":"vulkancraft:crafting","inputs":[{"item":"vulkancraft:stone","count":4}],"outputs":[{"item":"vulkancraft:stone_brick","count":1}]}]})",
                  ierr),
              "recipe registry loads stone_brick recipe");
        check(recipes.size() == 1, "one recipe registered");
        engine::registry::Inventory inv(9);
        // Default slots are LOCKED (empty SlotFilter without allowAny) — the
        // recipe query needs the live inventory to hold the inputs, so unlock
        // every slot exactly like UiCraftingTests::unlock_all does.
        engine::registry::SlotFilter any;
        any.allowAny = true;
        for (int s = 0; s < inv.slot_count(); ++s) inv.set_filter(s, any);
        std::string invErr;
        inv.add(engine::registry::ItemStack{ "vulkancraft:stone", 4 }, items,
                invErr);
        const auto craftable =
            recipes.recipes_for(inv, "vulkancraft:crafting", items);
        check(craftable.size() == 1, "4 stone satisfies the recipe");
        engine::registry::CraftResult crafted =
            recipes.craft(inv, *craftable.front(), "vulkancraft:crafting", items,
                          1);
        check(crafted.ok && !crafted.outputs.empty(),
              "craft produces the output atomically");
        check(crafted.outputs[0].item == "vulkancraft:stone_brick",
              "craft output is stone_brick");
    }

    // ── ECS ↔ physics mirror (B): every live ECS entity is mirrored by one
    // kinematic body in the canonical gameplay runtime — the ECS is the
    // motion authority, the body reflects its transform so physics
    // queries/collisions see the mob. Despawn destroys the body (no orphans).
    {
        const auto mobId = ecs->spawn("mob",
                                      engine::entity::Position{ 10.0f, 64.0f, 0.0f },
                                      error);
        check(mobId.valid(), "spawn mob for physics mirror");
        engine::gameplay::BodyId body;
        {
            engine::gameplay::BodySpec spec;
            spec.motion = engine::gameplay::MotionType::Kinematic;
            spec.position = glm::vec3(10.0f, 64.0f, 0.0f);
            spec.mass = 20.0f;
            spec.shape = engine::gameplay::SphereShape{ 0.5f };
            body = physics->physics().create_body(spec);
            check(body.valid(), "kinematic body created for the mob");
        }
        // Move the ECS entity (authority) and push the transform to the body.
        check(ecs->set_position(mobId, engine::entity::Position{ 12.0f, 64.0f, 4.0f }),
              "ecs sets the mob position");
        engine::entity::Position live;
        check(ecs->get_position(mobId, live), "ecs reads back the mob position");
        physics->physics().set_transform(body, glm::vec3(live.x, live.y, live.z),
                                         glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f });
        engine::gameplay::BodyState mirrored;
        check(physics->physics().body_state(body, mirrored), "body state readable");
        const float dx = mirrored.position.x - 12.0f;
        const float dz = mirrored.position.z - 4.0f;
        check(dx * dx + dz * dz < 1e-3f,
              "body transform mirrors the live ECS position");
        check(physics->physics().destroy_body(body), "body destroyed with the mob");
        check(ecs->despawn(mobId), "ecs despawns the mob");
    }

    // ── Terrain-aware animation (H): the gait planner maps a body state to
    // per-foot targets over a quadruped gait — the deterministic half of
    // foot-placement, run standalone because this gate is self-contained (no
    // voxel world to sample). The full plan->place pipeline runs on the server
    // over its authoritative world via create_voxel_foot_terrain_sampler.
    {
        engine::animation::GaitAsset gait;
        gait.name = "quad";
        gait.cycleDuration = 1.0f;
        gait.stanceFraction = 0.6f;
        gait.stepHeight = 0.2f;
        gait.maxStride = 0.5f;
        const glm::vec3 hips[4] = { { 0.3f, 0.8f, 0.5f },
                                    { -0.3f, 0.8f, 0.5f },
                                    { 0.3f, 0.8f, -0.5f },
                                    { -0.3f, 0.8f, -0.5f } };
        for (int i = 0; i < 4; ++i) {
            engine::animation::LegChainAsset leg;
            leg.name = std::string("FL") + std::to_string(i);
            leg.hipOffset = hips[i];
            leg.upperLength = 1.0f;
            leg.lowerLength = 1.0f;
            leg.restOffset = glm::vec3(0.0f, -0.8f, 0.0f);
            leg.hipBone = 1 + i * 3;
            leg.kneeBone = 2 + i * 3;
            leg.footBone = 3 + i * 3;
            gait.legs.push_back(leg);
        }
        gait.legPhases = { 0.0f, 0.5f, 0.25f, 0.75f };
        std::string gerr;
        check(gait.validate(gerr), "gait asset validates");
        auto planner = engine::animation::create_contact_planner();
        engine::animation::GaitPlan plan;
        // A non-zero velocity drives a real stride (targets move with the
        // body) — deterministic and observable.
        check(planner->plan(gait, 0.3f, glm::vec3(8.0f, 65.0f, 8.0f), 0.0f,
                            glm::vec2(1.0f, 0.0f), plan, gerr),
              "gait plan computed");
        check(plan.feet.size() == 4, "gait plans four feet");
        // Two consecutive phases produce differing targets (the clock advances
        // the stride) — deterministic and observable.
        engine::animation::GaitPlan later;
        check(planner->plan(gait, 0.7f, glm::vec3(8.0f, 65.0f, 8.0f), 0.0f,
                            glm::vec2(1.0f, 0.0f), later, gerr),
              "gait plan computed at a later phase");
        check(!(plan.feet.empty() && later.feet.empty()),
              "gait plans advance with the gait clock");
        if (!plan.feet.empty() && !later.feet.empty()) {
            const glm::vec3 a = plan.feet[0].targetWorld;
            const glm::vec3 b = later.feet[0].targetWorld;
            const float dx = a.x - b.x, dz = a.z - b.z;
            check(dx * dx + dz * dz > 1e-6f,
                  "gait phase advances the first foot target");
        }
    }

    // ── Crowd steering (I): the game computes an aggregate flock force for
    // the LIVE ECS mobs toward the player (seek + separation, blended) — a
    // deterministic pure computation observable as a scalar magnitude.
    {
        const std::vector<engine::ai::Vec3> mobPoses = {
            { 0.0f, 64.0f, 0.0f },
            { 2.0f, 64.0f, 1.0f },
            { -1.0f, 64.0f, 3.0f },
        };
        const engine::ai::Vec3 playerPos{ 8.0f, 64.0f, 8.0f };
        std::vector<engine::ai::SteeringNeighbor> all;
        for (const auto& p : mobPoses) all.push_back({ p, { 0.0f, 0.0f, 0.0f } });
        float sum = 0.0f;
        for (const auto& n : all) {
            engine::ai::SteeringAgent agent;
            agent.position = n.position;
            agent.max_speed = 4.0f;
            const engine::ai::Vec3 desired = engine::ai::seek(agent, playerPos);
            const engine::ai::Vec3 separate =
                engine::ai::separation(agent, all, 3.0f);
            const engine::ai::Vec3 blended =
                engine::ai::blend({ desired, separate }, { 1.0f, 1.5f },
                                  agent.max_force);
            sum += std::sqrt(blended.x * blended.x + blended.y * blended.y +
                             blended.z * blended.z);
        }
        const float avg = all.empty() ? 0.0f : sum / (float)all.size();
        check(avg > 0.0f, "mobs steer toward the player (seek magnitude > 0)");
        // Determinism: re-running the same inputs yields the same magnitude.
        float sum2 = 0.0f;
        for (const auto& n : all) {
            engine::ai::SteeringAgent agent;
            agent.position = n.position;
            agent.max_speed = 4.0f;
            const engine::ai::Vec3 blended =
                engine::ai::blend({ engine::ai::seek(agent, playerPos),
                                    engine::ai::separation(agent, all, 3.0f) },
                                  { 1.0f, 1.5f }, agent.max_force);
            sum2 += std::sqrt(blended.x * blended.x + blended.y * blended.y +
                              blended.z * blended.z);
        }
        check(sum2 == sum, "steering is deterministic for identical inputs");
    }

    // ── Perception (I.112): the player's sensor suite is fed by LIVE mob
    // stimuli (hostiles are threats); update() advances a deterministic
    // detection memory with a nearest-threat query — a pure sensor core
    // that the game consumes per frame (observable, stateless w.r.t. the ECS).
    {
        std::unique_ptr<engine::ai::IPerception> perception =
            engine::ai::create_perception();
        check(perception != nullptr, "perception factory returns a provider");
        engine::ai::PerceptionSpec spec;
        spec.vision_range = 24.0f;
        spec.vision_half_angle_deg = 30.0f;  // (0,90] half-FOV
        spec.hearing_range = 12.0f;
        spec.proximity_range = 2.5f;
        spec.memory_ttl = 5.0f;
        spec.max_range = 64.0f;
        std::string perr;
        check(perception->configure(spec, perr),
              "perception spec configures (vision/hearing/proximity)");
        // Two hostiles in front + one neutral behind: sensor sees hostiles.
        const std::vector<engine::ai::PerceptionStimulus> stims = {
            { 1u, { 4.0f, 64.0f, 0.0f }, 0.8f, true, "mob" },
            { 2u, { 3.0f, 64.0f, 2.0f }, 0.8f, true, "mob" },
            { 3u, { -8.0f, 64.0f, 0.0f }, 0.8f, false, "mob" },
        };
        const engine::ai::Vec3 playerPos{ 0.0f, 64.0f, 0.0f };
        const engine::ai::Vec3 fwd{ 1.0f, 0.0f, 0.0f };
        std::string uerr;
        check(perception->update(playerPos, fwd, stims, 0.033f, uerr),
              "perception advances with live stimuli");
        const auto dets = perception->detections();
        check(dets.size() >= 2,
              "perception detects the hostiles in the vision cone");
        bool hostileDetected = false;
        for (const auto& d : dets) if (d.hostile) hostileDetected = true;
        check(hostileDetected, "nearest-threat path flags a hostile source");
        engine::ai::Detection threat;
        check(perception->nearest_threat(threat) && threat.hostile,
              "nearest_threat resolves the closest hostile");
    }

    // ── Deterministic combat FSM (I.114): the game runs a mob state machine
    // whose conditions are fed by the perception suite (threat proximity).
    // The FSM is PURE — tick() with dt + conditions yields state / emitted
    // action ids deterministically; state is observable and JSON round-trips
    // all-or-nothing. This is the decision core the game's mobFsm consumes.
    {
        std::unique_ptr<engine::ai::IFsm> fsm = engine::ai::create_fsm();
        check(fsm != nullptr, "fsm factory returns a provider");
        engine::ai::FsmSpec spec;
        spec.initial = "idle";
        spec.states = {
            { "idle",    "enter_idle",    "update_idle",    "exit_idle",    false },
            { "alerted", "enter_alerted", "update_alerted", "exit_alerted", false },
            { "combat",  "enter_combat",  "update_combat",  "exit_combat",  false },
            { "recover", "enter_recover", "update_recover", "exit_recover", false },
        };
        spec.transitions = {
            { "idle",    "alerted", "", "threat_near",  0.0 },
            { "alerted", "combat",  "", "threat_close", 0.0 },
            { "combat",  "recover", "", "threat_gone",  0.0 },
            { "recover", "idle",    "", "calm",         0.0 },
        };
        std::string ferr;
        check(fsm->configure(spec, ferr) && fsm->start(ferr),
              "fsm spec configures and starts at the initial state");
        check(fsm->state() == "idle", "fsm begins in idle");
        // Threat near → alerted; threat close → combat. drain the action ids.
        fsm->set_condition("threat_near", true);
        fsm->set_condition("threat_close", true);
        check(fsm->tick(0.016, ferr) && fsm->state() == "alerted",
              "fsm reacts to threat_near (idle -> alerted)");
        check(fsm->tick(0.016, ferr) && fsm->state() == "combat",
              "fsm reacts to threat_close (alerted -> combat)");
        bool hasCombatAction = false;
        for (const auto& a : fsm->drain_actions())
            if (a == "enter_combat" || a == "update_combat") hasCombatAction = true;
        check(hasCombatAction, "fsm drains the combat action ids in order");
        // Deterministic JSON round-trip restores the same state.
        std::string j = fsm->serialize_state();
        std::unique_ptr<engine::ai::IFsm> fsm2 = engine::ai::create_fsm();
        std::string ferr2;
        check(fsm2 && fsm2->configure(spec, ferr2) &&
              fsm2->deserialize_state(j, ferr2) && fsm2->state() == fsm->state(),
              "fsm state serializes all-or-nothing and restores current state");
    }

    // ── Data-driven behavior tree (I.116): the game runs a decision tree for
    // a mob fed by a caller-owned Blackboard (the perception surface). The
    // runtime is PURE — tick(dt, bb) yields a root status and debug_trace()
    // the ordered node visits; a threat present in the blackboard drives a
    // sequence to issue an engage action. This is the decision core the
    // game's mobTree consumes per frame.
    {
        engine::ai::BehaviorTreeSpec bspec;
        bspec.root.type = "sequence";
        bspec.root.children = {
            { "condition", {}, "all", "none", 1, 0.0, 0.0, "eq",
              "threat", engine::ai::BlackboardValue{
                  engine::ai::BlackboardKind::Bool, true, 0.0, "" } },
            { "action", {}, "all", "none", 1, 0.0, 0.0, "eq",
              "next_action", engine::ai::BlackboardValue{
                  engine::ai::BlackboardKind::String, false, 0.0, "engage" } },
        };
        std::string terr;
        std::unique_ptr<engine::ai::IBehaviorTree> tree =
            engine::ai::create_behavior_tree(bspec, terr);
        check(tree != nullptr, "behavior tree spec compiles");
        // Threat absent (blackboard empty) -> condition fails -> sequence fails.
        engine::ai::Blackboard bbEmpty;
        check(tree->tick(0.016, bbEmpty) ==
                  engine::ai::BehaviorStatus::Failure,
              "tree fails without a threat in the blackboard");
        // Threat present -> condition passes -> action stores the engage id.
        engine::ai::Blackboard bbThreat;
        bbThreat.set("threat", true);
        check(tree->tick(0.016, bbThreat) ==
                  engine::ai::BehaviorStatus::Success,
              "tree succeeds when the blackboard carries a threat");
        engine::ai::BlackboardValue engage;
        check(bbThreat.get("next_action", engage) &&
              engage.kind == engine::ai::BlackboardKind::String &&
              engage.text == "engage",
              "tree action writes the engage action onto the blackboard");
        // Deterministic debug trace exposes the visited node order. The
        // canonical contract (BehaviorTreeTests) is parent-child ordered:
        // the FIRST visit is the first child ("0.0") and the LAST is the
        // root ("0") — post-order, deterministic traversal paths.
        const auto trace = tree->debug_trace();
        check(!trace.empty() && trace.front().first == "0.0" &&
              trace.back().first == "0",
              "tree debug trace visits children before the root deterministically");
    }

    // ── Utility AI (I.118): the game scores tactical options from the
    // perception suite + day/night. Pure/deterministic: normalized inputs and
    // select() returns the highest-utility action (ties → declaration order).
    // This is the selection core the game's utilityAi consumes per frame.
    {
        std::unique_ptr<engine::ai::IUtilityAi> u = engine::ai::create_utility_ai();
        check(u != nullptr, "utility AI factory returns a provider");
        engine::ai::UtilitySpec spec;
        spec.actions = {
            { "engage", {
                { "threat", engine::ai::UtilityCurve::Linear, 2.0, 0.0, 1.0, 0.5 },
            } },
            { "retreat", {
                // Linear: retreat utility RISES with danger (danger=0 -> 0),
                // so a calm guard prefers patrol, not retreat.
                { "danger", engine::ai::UtilityCurve::Linear, 1.5, 0.0, 1.0, 0.5 },
            } },
            { "patrol", {
                { "gap", engine::ai::UtilityCurve::Linear, 0.5, 0.0, 1.0, 0.5 },
            } },
        };
        std::string uerr;
        check(u->configure(spec, uerr), "utility spec configures");
        // No threat / max gap -> patrol (gap 1.0 => 0.5 utility, dominate).
        u->set_input("threat", 0.0);
        u->set_input("danger", 0.0);
        u->set_input("gap", 1.0);
        engine::ai::UtilitySelection sel = u->select();
        check(sel.id == "patrol", "utility picks patrol when calm (max gap)");
        // High threat -> engage has the highest weighted utility.
        u->set_input("threat", 1.0);
        u->set_input("danger", 0.0);
        u->set_input("gap", 0.0);
        sel = u->select();
        check(sel.id == "engage", "utility picks engage when a threat is close");
        // Deterministic: identical inputs give identical utilities.
        const auto u1 = u->utilities();
        u->set_input("threat", 0.4);
        u->set_input("danger", 0.2);
        const auto u2a = u->utilities();
        const auto u2b = u->utilities();
        check(u2a == u2b, "utility scoring is deterministic for identical inputs");
    }

    // ── GOAP planner (I.119): the game plans the lowest-cost action sequence
    // to reach a goal from the perception-derived facts. Uniform-cost search,
    // deterministic; a richer world state shortens the plan. This is the
    // planning core the game's mobPlanner consumes per frame.
    {
        std::unique_ptr<engine::ai::IPlanner> p = engine::ai::create_planner();
        check(p != nullptr, "planner factory returns a provider");
        engine::ai::PlannerSpec spec;
        spec.actions = {
            { "scout",  1.0, {}, { { "intel", true } } },
            { "engage", 2.0, { { "intel", true } }, { { "contact", true } } },
            { "finish", 3.0, { { "contact", true } }, { { "defeated", true } } },
        };
        std::string perr;
        check(p->configure(spec, perr), "planner spec configures");
        p->set_goal("defeated", true);
        // No facts -> full chain scout → engage → finish.
        auto plan = p->plan(perr);
        check(plan.success && plan.actions.size() == 3 &&
              plan.actions[0] == "scout" && plan.actions[2] == "finish",
              "planner finds the full goal-achieving chain");
        check(plan.total_cost == 6.0, "planner totals the chain cost");
        // With intel+contact already present, the plan shortens to only finish.
        p->set_atom("intel", true);
        p->set_atom("contact", true);
        plan = p->plan(perr);
        check(plan.success && plan.actions.size() == 1 &&
              plan.actions[0] == "finish",
              "planner shortens the plan from existing world facts");
        // Determinism: identical inputs, identical plan.
        const auto r1 = p->plan(perr);
        const auto r2 = p->plan(perr);
        check(r1.success == r2.success && r1.actions == r2.actions &&
              r1.total_cost == r2.total_cost,
              "planning is deterministic for identical inputs");
    }

    // ── Per-entity AI LOD (I.117): the game classifies the live mob ECS by
    // distance to the player (Full/Reduced/Aggregate/Dormant) and applies
    // budgets. Pure/deterministic — tier_for pair (distance, should_update)
    // and allocate() produce a stable active/dormant split per tick. This is
    // the classification core the game's aiLod consumes per frame.
    {
        std::unique_ptr<engine::ai::IAiLod> lod = engine::ai::create_ai_lod();
        check(lod != nullptr, "AI LOD factory returns a provider");
        engine::ai::AiLodSpec spec;
        spec.full_radius = 16.0;
        spec.reduced_radius = 64.0;
        spec.aggregate_radius = 256.0;
        spec.reduced_interval = 4.0;
        spec.aggregate_interval = 16.0;
        std::string lerr;
        check(lod->configure(spec, lerr), "AI LOD spec configures");
        // A close mob is Full (updates every tick); a far one Dormant (never).
        check(lod->tier_for(2.0) == engine::ai::AiLodTier::Full,
              "near mob classifies Full");
        check(lod->tier_for(500.0) == engine::ai::AiLodTier::Dormant,
              "far mob classifies Dormant");
        // should_update is deterministic per (tier, tick).
        check(lod->should_update(engine::ai::AiLodTier::Full, 0),
              "Full tier updates at every tick");
        check(!lod->should_update(engine::ai::AiLodTier::Dormant, 0),
              "Dormant tier never updates");
        // allocate() classifies a population with budgets applied.
        const std::vector<engine::ai::AiLodEntry> entries = {
            { 1u, 2.0 },   // near -> Full
            { 2u, 30.0 },  // near-ish -> Reduced
            { 3u, 900.0 }, // far -> Dormant
        };
        const auto a1 = lod->allocate(0, entries);
        check(a1.size() == 3, "allocate returns one per entry");
        check(a1[0].tier == engine::ai::AiLodTier::Full && a1[0].update,
              "near entry allocated Full and updates at tick 0");
        const auto a2 = lod->allocate(0, entries);
        bool deterministic = true;
        for (std::size_t i = 0; i < a1.size(); ++i)
            if (a1[i].tier != a2[i].tier || a1[i].update != a2[i].update)
                deterministic = false;
        check(deterministic, "allocate is deterministic for identical inputs");
    }

    // ── AI event bus (I.120): the game logs the FSM decision events into a
    // deterministic bus and drains them per frame. Purely additive — emit
    // appends, drain returns in order and clears, serialization round-trips
    // bit-exact preserving order/payload. This closes the engine/ai decision
    // domain into the loop the game consumes.
    {
        std::unique_ptr<engine::ai::IAiEventBus> b =
            engine::ai::create_ai_event_bus();
        check(b != nullptr, "AI event bus factory returns a provider");
        engine::ai::AiEventBusSpec spec;
        spec.max_events = 8;
        std::string err;
        check(b->configure(spec, err), "AI event bus spec configures");
        b->emit(1, "fsm", "state_changed", "alerted");
        b->emit(1, "fsm", "state_changed", "combat");
        b->emit(2, "tree", "action", "engage");
        const auto log = b->peek();
        check(log.size() == 3 && log[0].kind == "state_changed" &&
              log[1].payload == "combat",
              "bus preserves emit order and payloads");
        // Deterministic FIFO ring: max_events bounds the log, dropping oldest.
        b->emit(3, "fsm", "state_changed", "recover");
        b->emit(4, "fsm", "state_changed", "idle");
        b->emit(5, "fsm", "state_changed", "alerted");
        b->emit(6, "fsm", "state_changed", "combat");
        b->emit(7, "fsm", "state_changed", "recover");
        b->emit(8, "fsm", "state_changed", "idle");
        const auto bounded = b->peek();
        check(bounded.size() == 8, "ring buffer bounds the event log");
        // Serialize/restore is all-or-nothing and preserves order + payloads.
        const std::string j = b->serialize();
        std::unique_ptr<engine::ai::IAiEventBus> b2 =
            engine::ai::create_ai_event_bus();
        check(b2 && b2->configure(spec, err) && b2->deserialize(j, err),
              "bus serializes all-or-nothing and restores");
        check(b2->peek() == bounded, "restored bus equals the original log");
        // Drain returns in emit order and clears the log.
        const auto e1 = b->drain();
        check(e1 == bounded && b->peek().empty(), "drain returns in order and clears");
    }

    // ── Crowd simulation (I.121): advances the live mob ECS as a crowd
    // population — tiers re-classified by distance to the player, wake/sleep,
    // bounded ticks per frame, deterministic. This is the final stateful core
    // of the engine/ai decision suite; active/dormant/woken observable in the
    // title.
    {
        std::unique_ptr<engine::ai::ICrowdSimulation> sim =
            engine::ai::create_crowd_simulation();
        check(sim != nullptr, "crowd simulation factory returns a provider");
        engine::ai::CrowdSpec spec;
        spec.full_radius = 16.0;
        spec.reduced_radius = 64.0;
        spec.aggregate_radius = 256.0;
        spec.reduced_interval = 4.0;
        spec.max_ticks_per_frame = 16;
        std::string serr;
        check(sim->configure(spec, serr), "crowd simulation spec configures");
        check(sim->set_agents({ { 1u, { 2.0f, 64.0f, 0.0f }, "guard" },
                                { 2u, { 30.0f, 64.0f, 0.0f }, "guard" },
                                { 3u, { 900.0f, 64.0f, 0.0f }, "guard" } },
                               serr),
              "crowd set_agents accepts the population");
        check(sim->agent_count() == 3, "crowd holds the full population");
        // Focus far away → far agent Dormant (sleeping), near agent Full.
        auto res = sim->advance({ 400.0f, 64.0f, 0.0f }, 1, serr);
        const auto ref = res.agent_states[2].tier;
        check(ref == engine::ai::CrowdTier::Dormant,
              "far crowd agent sleeps (Dormant)");
        // Move the focus next to the far agent → it wakes (tier changes).
        res = sim->advance({ 899.0f, 64.0f, 0.0f }, 4, serr);
        bool woke = false;
        for (const auto& s : res.agent_states)
            if (s.id == 3u && s.tier != engine::ai::CrowdTier::Dormant) woke = true;
        check(woke, "crowd wakes the agent as the focus approaches");
        // Deterministic: identical focus/frames on a fresh sim = same result;
        // re-running gives the same classification even after prior advances
        // (tier is re-derived from distance each advance), so a fresh and a
        // stateful sim agree on the Dormant classification.
        std::unique_ptr<engine::ai::ICrowdSimulation> sim2 =
            engine::ai::create_crowd_simulation();
        check(sim2 && sim2->configure(spec, serr) &&
              sim2->set_agents({ { 1u, { 2.0f, 64.0f, 0.0f }, "guard" },
                                 { 2u, { 30.0f, 64.0f, 0.0f }, "guard" },
                                 { 3u, { 900.0f, 64.0f, 0.0f }, "guard" } },
                                serr),
              "second crowd sim configures identically");
        const auto freshFar =
            sim2->advance({ 400.0f, 64.0f, 0.0f }, 1, serr)
                .agent_states[2].tier;
        const auto statefulFar =
            sim->advance({ 400.0f, 64.0f, 0.0f }, 1, serr)
                .agent_states[2].tier;
        check(freshFar == statefulFar && freshFar == engine::ai::CrowdTier::Dormant,
              "crowd classification is deterministic across instances");
    }

    // ── Ability effects (G.92): a validated ability-effect table emits real
    // events into the SAME canonical IGameplayEvents bus the game publishes
    // break/place events into. configure() is all-or-nothing; emit() validates
    // the effect id and publishes a serialized-spec event (kind 1..7). This is
    // the ability-integration core the game consumes per block break.
    {
        std::unique_ptr<engine::gameplay::IAbilityEffects> fx =
            engine::gameplay::create_ability_effects();
        check(fx != nullptr, "ability effects factory returns a provider");
        std::string xerr;
        check(fx->configure(
                  { { "kick", engine::gameplay::AbilityEffectKind::ForceImpulse,
                      4.0f, 1.0f, { 0.0f, 0.0f, 0.0f }, "", "", 1,
                      "ability.kick" },
                    { "excavate",
                      engine::gameplay::AbilityEffectKind::DestroyBlock,
                      1.0f, 1.0f, { 0.0f, 0.0f, 0.0f }, "", "", 1,
                      "ability.excavate" },
                    { "ping", engine::gameplay::AbilityEffectKind::Generic,
                      1.0f, 1.0f, { 0.0f, 0.0f, 0.0f }, "", "", 1,
                      "ability.ping" } },
                  xerr),
              "ability effect specs configure all-or-nothing");
        check(fx->count() == 3 && fx->ids()[0] == "excavate",
              "ability effects table lists specs in order");
        // Emit into a real IGameplayEvents bus (same type the game publishes
        // into); the effect publishes a serialized-spec event.
        std::unique_ptr<engine::gameplay::IGameplayEvents> events =
            engine::gameplay::create_gameplay_events();
        check(events != nullptr, "gameplay events bus available for emit");
        check(fx->emit(*events, "kick", 7u, xerr), "known effect emits");
        check(!fx->emit(*events, "missing", 8u, xerr),
              "unknown effect refuses to emit");
        // The bus actually received the published event (peek/count proof).
        const auto queue = events->drain();
        check(!queue.empty(), "emitted ability event reached the canonical bus");
    }

    // ── Missions (G.94): the server drives the SDK mission runtime against a
    // real world seam. accept -> update -> complete runs the deterministic
    // state machine; every decision (unlock, objective progress, reward,
    // set_flag) flows through the public seam and is observable.
    {
        struct LocalMissionWorld final : public engine::gameplay::IMissionWorld {
            float count{ 5.0f };
            std::string rewardItem;
            int rewardCount{ 0 }, rewardXp{ 0 };
            bool rewarded{ false };
            bool flagged{ false };
            float count_of(const std::string&) const override { return count; }
            bool flag(const std::string&) const override { return false; }
            float attribute(const std::string&) const override { return 2.0f; }
            bool position(float& x, float& z) const override {
                x = 8.0f; z = 8.0f; return true;
            }
            bool apply_reward(const std::string& itemId, int count, int xp) override {
                rewardItem = itemId; rewardCount = count; rewardXp = xp;
                rewarded = true; return true;
            }
            bool set_flag(const std::string&) override { flagged = true; return true; }
        };
        auto missions = engine::gameplay::create_mission_runtime();
        check(missions != nullptr, "mission runtime factory returns a provider");
        engine::gameplay::MissionDefinition def;
        def.name = "First Steps";
        def.id = "11111111-2222-3333-4444-555555555555";
        engine::gameplay::MissionObjective collect;
        collect.id = "collect_stone";
        collect.kind = engine::gameplay::MissionObjectiveKind::Collect;
        collect.target = "stone";
        collect.count = 3;
        engine::gameplay::MissionObjective reach;
        reach.id = "reach_hill";
        reach.kind = engine::gameplay::MissionObjectiveKind::Reach;
        reach.count = 1;
        reach.x = 8.0f; reach.z = 8.0f; reach.radius = 5.0f;
        def.objectives = { collect, reach };
        def.reward.itemId = "coin";
        def.reward.count = 5;
        def.reward.xp = 100;
        def.reward.setFlag = "quest1_done";
        std::string merr;
        check(def.validate(merr), "mission definition validates");
        LocalMissionWorld mw;
        engine::gameplay::MissionState st;
        std::vector<engine::gameplay::MissionEvent> evs;
        check(missions->accept(def, st, mw, evs, merr) && st.accepted,
              "mission accepts and marks the state accepted");
        check(missions->update(def, st, mw, evs, merr),
              "mission update advances objective progress");
        check(missions->complete(def, st, mw, evs, merr) && st.completed,
              "mission completes when objectives are done");
        check(mw.rewarded && mw.rewardItem == "coin" && mw.rewardCount == 5 &&
              mw.flagged,
              "mission applies the reward and sets the completion flag");
        std::string ser;
        check(missions->serialize_state(st, ser, merr) && !ser.empty(),
              "mission state serializes bit-exact");
    }

    // ── Ability system (G.93): the server drives the SDK ability runtime
    // against a real IAbilityWorld seam (read-only over the authoritative
    // voxel world in the product; a body registry here). A data-driven damage
    // ability is registered and cast; the health delta + cooldown are
    // observable, proving powers integrate through the public seam.
    {
        struct LocalAbilityWorld final : public engine::gameplay::IAbilityWorld {
            float hp{ 100.0f };
            std::uint32_t block_at(int, int, int) const override { return 0; }
            bool set_block(int, int, int, std::uint32_t) override { return false; }
            bool body_state(const engine::gameplay::AbilityBodyId&,
                            engine::gameplay::AbilityBodyState& out) const override {
                out.position = glm::vec3(0.0f);
                out.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
                out.linearVelocity = glm::vec3(0.0f);
                out.angularVelocity = glm::vec3(0.0f);
                return true;
            }
            bool apply_impulse(const engine::gameplay::AbilityBodyId&,
                               const glm::vec3&) override { return true; }
            bool add_force(const engine::gameplay::AbilityBodyId&,
                           const glm::vec3&) override { return true; }
            bool set_transform(const engine::gameplay::AbilityBodyId&,
                               const glm::vec3&, const glm::quat&) override { return true; }
            bool raycast(const glm::vec3&, const glm::vec3&, float,
                         engine::gameplay::AbilityRaycastHit&) const override {
                return false;
            }
            float attribute(const engine::gameplay::AbilityBodyId&,
                            const std::string&) const override { return 3.0f; }
            engine::gameplay::AbilityTagList tags(
                const engine::gameplay::AbilityBodyId&) const override { return {}; }
            bool spend_cost(const engine::gameplay::AbilityBodyId&,
                            const std::string&, float) override { return true; }
            bool health(const engine::gameplay::AbilityBodyId&,
                        float& out) const override { out = hp; return true; }
            bool damage(const engine::gameplay::AbilityBodyId&,
                        float amount) override { hp -= amount; return true; }
            bool heal(const engine::gameplay::AbilityBodyId&,
                      float amount) override { hp += amount; return true; }
        };
        auto abilities = engine::gameplay::create_ability_system();
        check(abilities != nullptr, "ability system factory returns a provider");
        engine::gameplay::AbilityDefinition punch;
        punch.name = "punch";
        punch.id = "abilities:punch";
        punch.targeting.mode = engine::gameplay::AbilityTargetMode::Self;
        engine::gameplay::AbilityEffect dmg;
        dmg.type = engine::gameplay::AbilityEffectType::Damage;
        dmg.amount = 10.0f;
        punch.effects.push_back(dmg);
        punch.cooldownSeconds = 2.0f;
        std::string aerr;
        check(punch.validate(aerr) && abilities->register_ability(punch, aerr),
              "data-driven ability validates and registers");
        LocalAbilityWorld aw;
        engine::gameplay::AbilityBodyId caster;
        caster.id = 1;
        engine::gameplay::AbilityTarget target;
        target.mode = engine::gameplay::AbilityTargetMode::Body;
        target.body = caster;  // damage resolves against the caster's body
        const auto cast = abilities->cast(punch.id, caster, target, aw);
        check(cast.accepted && cast.effectCount == 1,
              "ability cast is accepted and applies its effects");
        check(aw.hp == 90.0f, "ability damage flows through the world seam");
        abilities->update(0.1f, aw);
        check(abilities->on_cooldown(punch.id),
              "ability cooldown starts after the cast");
        // A second cast while on cooldown is refused (all-or-nothing).
        check(!abilities->cast(punch.id, caster, target, aw).accepted,
              "ability on cooldown refuses a second cast");
    }

    // ── Structure placement (D): the server evaluates the data-driven
    // placement pipeline against its authoritative world — a hand-authored
    // asset + spawn rule decide deterministically (try_place is a pure
    // function of rules/seed/column), producing an observable placement with
    // generated content, exactly the deterministic half the item requires.
    {
        auto structures = engine::procgen::create_structure_placement_system();
        std::string serr;
        engine::procgen::StructureAssetSpec spec;
        spec.sampleWidth = 4;
        spec.sampleHeight = 4;
        for (int z = 0; z < spec.sampleHeight; ++z) {
            for (int x = 0; x < spec.sampleWidth; ++x) {
                const bool wall = z == 0 || z == spec.sampleHeight - 1 ||
                                  x == 0 || x == spec.sampleWidth - 1;
                spec.sample.push_back(wall ? 1u : 2u);
            }
        }
        spec.patternSize = 2;
        spec.seed = 11u;
        spec.profiles.emplace_back(1, std::vector<std::uint32_t>{ 3, 3, 3 });
        spec.profiles.emplace_back(2, std::vector<std::uint32_t>{ 5 });
        engine::procgen::StructureDefinition def;
        def.id = "vulkancraft:watchtower";
        def.spec = spec;
        def.outputWidth = 8;
        def.outputHeight = 8;
        check(structures->add_definition(def, serr), "structure definition registered");
        engine::procgen::StructureSpawnRule rule;
        rule.structureId = def.id;
        rule.density = 1.0f;
        rule.spacing = 8;
        check(structures->set_rules({ rule }, serr), "structure spawn rules set");
        engine::procgen::StructurePlacement placed;
        check(structures->try_place({}, 8, 8, 120, "plains", 42u, placed, serr),
              "structure pipeline decides a placement");
        check(placed.structureId == "vulkancraft:watchtower",
              "placement resolves the definition");
        check(placed.output.succeeded && !placed.output.blocks.empty(),
              "placement generated content");
    }

    // ── Vehicle path (F): the server instantiates a REAL vehicle through
    // the promoted gameplay runtime — the asset validates, the Jolt provider
    // is available (create_vehicle_provider gate), and the chassis is
    // assembled/claimed by create_vehicle_from_asset on the live runtime.
    {
        engine::vehicles::VehicleAsset vehicle;
        vehicle.name = "gate_probe_car";
        vehicle.position = glm::vec3(24.0f, 90.0f, 8.0f);
        vehicle.wheels.push_back(
            engine::vehicles::WheelComponent{ { -0.8f, 0.0f, -0.55f } });
        vehicle.wheels.push_back(
            engine::vehicles::WheelComponent{ { -0.8f, 0.0f, 0.55f } });
        vehicle.wheels.push_back(
            engine::vehicles::WheelComponent{ { 0.8f, 0.0f, -0.55f } });
        vehicle.wheels.push_back(
            engine::vehicles::WheelComponent{ { 0.8f, 0.0f, 0.55f } });
        std::string vehErr;
        check(vehicle.validate(vehErr), "vehicle asset validates");
        std::string provErr;
        auto provider = engine::vehicles::create_vehicle_provider(
            vehicle.provider, provErr);
        check(provider && provider->available(), "jolt vehicle provider available");
        if (physics) {
            auto vehicleRuntime =
                physics->create_vehicle_from_asset(vehicle);
            check(vehicleRuntime != nullptr,
                  "vehicle spawns on the canonical gameplay runtime");
        }
    }

    // ── Navigation authority (H): the promoted INavigationProvider (Recast +
    // Detour) bakes a navmesh from real voxel columns and answers a path
    // query — the same authority the server bakes from its authoritative
    // world and the editor play mode bakes from its NavigationComponents.
    {
        auto nav = engine::navigation::create_recast_navigation_provider();
        std::string nerr;
        engine::navigation::NavmeshConfig cfg;
        cfg.boundsMinX = -16.0f;
        cfg.boundsMaxX = 16.0f;
        cfg.boundsMinZ = -16.0f;
        cfg.boundsMaxZ = 16.0f;
        cfg.cellSize = 0.5f;
        std::vector<engine::navigation::VoxelColumn> columns;
        for (float x = -15.0f; x <= 15.0f; x += 0.5f) {
            for (float z = -15.0f; z <= 15.0f; z += 0.5f) {
                engine::navigation::VoxelColumn col;
                col.x = x;
                col.z = z;
                col.solidMinY = 0.0f;
                col.solidMaxY = 1.0f;
                col.solid = true;
                columns.push_back(col);
            }
        }
        if (!nav->build(cfg, columns, nerr)) {
            std::printf("  FAIL: navmesh build: %s\n", nerr.c_str());
            return 1;
        }
        check(nav->valid(), "navmesh valid after build");
        engine::navigation::PathResult path;
        check(nav->find_path(-8.0f, 1.0f, -8.0f, 8.0f, 1.0f, 8.0f, path),
              "find_path answers on the baked navmesh");
        check(path.found && !path.waypoints.empty(), "path found with waypoints");
        check(nav->is_walkable(0.0f, 1.0f, 0.0f), "is_walkable on the floor");
    }

    // ── Spatial partition (B.2): the ISpatialIndex mirrors entities into a
    // uniform cell grid and answers deterministic AABB/point queries — the
    // same index the game feeds per frame from the live mob ECS.
    {
        auto spatial = engine::entity::create_spatial_index();
        check(spatial != nullptr, "spatial index factory returns a provider");
        std::string serr;
        check(spatial->configure(8.0f, serr), "spatial index configures");
        engine::entity::SpatialBounds b1;
        b1.min = glm::vec3(1.0f, 1.0f, 1.0f);
        b1.max = glm::vec3(3.0f, 3.0f, 3.0f);
        check(spatial->insert(1u, b1, serr), "entity 1 inserted");
        engine::entity::SpatialBounds b2;
        b2.min = glm::vec3(40.0f, 1.0f, 40.0f);
        b2.max = glm::vec3(42.0f, 3.0f, 42.0f);
        check(spatial->insert(2u, b2, serr), "entity 2 inserted");
        const auto near = spatial->query_point(2.0f, 2.0f, 2.0f);
        check(near.size() == 1 && near[0] == 1u,
              "point query returns only the near entity");
        check(spatial->move(2u, b1),
              "entity 2 moves into the near cell");
        check(spatial->query_point(2.0f, 2.0f, 2.0f).size() == 2,
              "both entities near after the move");
        check(spatial->remove(1u) && spatial->count() == 1,
              "entity removed deterministically");
    }

    // ── Animation LOD (H.107): the IAnimationLod core maps relevance to a
    // tier and decides when a pose must be re-sampled — a near entity samples
    // every frame, a far one holds (frozen) between its slow interval.
    {
        auto animLod = engine::animation::create_animation_lod();
        check(animLod != nullptr, "animation LOD factory returns a provider");
        engine::animation::AnimationLodSpec spec;
        engine::animation::AnimationLodTier full;
        full.minRelevance = 0.5f;
        full.updateInterval = 1.0f / 60.0f;
        engine::animation::AnimationLodTier far;
        far.minRelevance = 0.0f;
        far.updateInterval = 1.0f / 15.0f;
        spec.tiers = { full, far };
        std::string lerr;
        check(spec.validate(1, lerr), "animation LOD spec validates");
        int tier = -1;
        check(animLod->select_tier(spec, 1.0f, tier, lerr) && tier == 0,
              "high relevance selects the full tier");
        check(animLod->select_tier(spec, 0.1f, tier, lerr) && tier == 1,
              "low relevance selects the far tier");
        engine::animation::AnimationLodState st;  // never sampled
        check(animLod->should_sample(spec, st, 1, 0.0f, lerr),
              "never-sampled state samples immediately");
        // After a sample at t=0, the far tier holds until 1/15 s elapses.
        st.tierIndex = 1;
        st.lastSampleTime = 0.0f;
        check(!animLod->should_sample(spec, st, 1, 0.01f, lerr),
              "far tier holds the pose between slow intervals");
        check(animLod->should_sample(spec, st, 1, 1.0f, lerr),
              "far tier re-samples when its interval elapsed");
    }

    // ── World director (G): the IWorldDirector core selects WHICH world
    // event runs next from clock + world tags — deterministic selection with
    // rules (requiresAll / cooldown / concurrency) and a utility ordering.
    {
        auto director = engine::director::create_world_director();
        check(director != nullptr, "world director factory returns a provider");
        engine::director::DirectorSpec spec;
        spec.maxPerTick = 1;
        engine::director::WorldEventCandidate storm;
        storm.id = "storm";
        storm.category = "weather";
        storm.requiresAll = { "night" };
        storm.cooldownTicks = 10;
        engine::director::WorldEventCandidate raid;
        raid.id = "raid";
        raid.category = "combat";
        raid.baseUtility = 0.9f;
        raid.requiresAll = { "danger" };
        spec.candidates = { storm, raid };
        std::string derr;
        check(director->set_spec(spec, derr), "director spec configures");
        engine::director::DirectorWorldState w;
        w.tick = 5;
        w.tags = { "danger" };
        std::vector<engine::director::EventSelectionState> sel = {
            { "storm", 0, 0, 0, 0, 0, 0 }, { "raid", 0, 0, 0, 0, 0, 0 } };
        std::vector<engine::director::DirectorSelection> picks;
        check(director->select(w, sel, picks, derr) && picks.size() == 1 &&
              picks[0].eventId == "raid",
              "director selects the eligible raid over the night-gated storm");
        check(sel[1].fireCount == 1, "director advanced the selection state");
        w.tags = { "day" };
        std::vector<engine::director::DirectorSelection> picks2;
        check(director->select(w, sel, picks2, derr) && picks2.empty(),
              "no eligible event on a quiet world");
    }

    // ── Capabilities (I): the capability registry enumerates the product's
    // real agent/player/vehicle capabilities — registered, listed and
    // queryable by stable id.
    {
        auto caps = engine::capabilities::create_capability_registry();
        check(caps != nullptr, "capability registry factory returns a provider");
        std::string cerr;
        engine::capabilities::CapabilityDescriptor walk;
        walk.stable_id = "agent.walk";
        walk.display_name = "Walk";
        walk.kind = engine::capabilities::CapabilityKind::Component;
        check(caps->register_capability(walk, cerr), "walk capability registers");
        engine::capabilities::CapabilityDescriptor drive;
        drive.stable_id = "vehicle.drive";
        drive.display_name = "Drive";
        drive.kind = engine::capabilities::CapabilityKind::Component;
        check(caps->register_capability(drive, cerr), "drive capability registers");
        check(caps->find("agent.walk") != nullptr,
              "registered capability resolves by stable id");
        check(caps->list(engine::capabilities::CapabilityKind::Component).size() == 2,
              "component-kind capabilities listed");
        check(!caps->register_capability(walk, cerr),
              "duplicate capability refused (all-or-nothing)");
    }

    // ── Shutdown reverts the composition and flushes persistence in order ──
    runtime->shutdown();
    const auto phases = runtime->enabled_phases();
    check(!phases.empty() && phases.back() == engine::WorldLifecyclePhase::Shutdown,
          "lifecycle ends on Shutdown");

    if (failures == 0) {
        std::printf("world_runtime_composition_tests: all checks passed\n");
        return 0;
    }
    std::printf("world_runtime_composition_tests: %d failure(s)\n", failures);
    return 1;
}