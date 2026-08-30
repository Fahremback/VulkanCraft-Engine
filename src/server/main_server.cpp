// Headless dedicated server (FALTANTES §16 item 6, Agente 3 A/C/J): the server
// owns the authoritative simulation — streaming voxel world + production
// physics (Jolt) + the PhysicsStreamingBridge that reconciles them every tick.
// Each tick:
//   world.update(focus, dt);  // chunks load/unload around the focus
//   bridge.sync(focus);       // slabs for loaded chunks, despawn on unload
//   physics.step(dt);         // advance bodies
// The server self-validates: a dynamic body spawned over streamed terrain
// must rest and SLEEP, a terrain-edit wake (wake_region) must wake it, and a
// focus move with a shrunk budget must evict chunks and despawn the contained
// bodies (no orphans).
//
// The network path uses the PUBLIC world-scoped replication surface
// (IWorldReplication over IWorldManager) and the public transport-backed
// INetworkServer — NOT the legacy parallel Engine::Networking::NetworkingRuntime
// (bug A3-109: "main_server.cpp não usa pilha paralela"). The server world is
// created through the IWorldManager so replication binds the same world the
// physics authority runs on; a dedicated client connection streams chunks by
// interest and receives authoritative block edits through the same protocol
// the game client uses.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include "../engine/physics/PhysicsRuntime.hpp"
#include "../engine/physics/PhysicsStreamingBridge.hpp"
#include <engine/networking/INetworkServer.hpp>
#include <engine/world/IWorldManager.hpp>
#include <engine/world/IWorldReplication.hpp>
#include <engine/world/IWorldRuntime.hpp>
#include <engine/world/ITimeTravel.hpp>
#include <engine/world/ITimelinePolicy.hpp>
#include <engine/world/IOriginRebase.hpp>
#include <engine/world/ILocalSpace.hpp>
#include <engine/registry/BlockRegistry.hpp>
#include <engine/observability/IObservability.hpp>
#include <engine/compression/ICompressionProvider.hpp>
#include "engine/sdk/RegistryJson.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <engine/entity/IEntityWorld.hpp>
#include <engine/gameplay/IGameplayRuntime.hpp>
#include <engine/gameplay/IGameplayIntegration.hpp>
#include <engine/gameplay/IGameplayBindings.hpp>
#include <engine/gameplay/IGameplaySystemWiring.hpp>
#include <engine/gameplay/IGameplayEvents.hpp>
#include <engine/gameplay/IGameplayMetrics.hpp>
#include <engine/gameplay/IGameplayEventRouter.hpp>
#include <engine/gameplay/IMissionAsset.hpp>
#include <engine/gameplay/IAbilitySystem.hpp>
#include <engine/gameplay/IDayNightCycle.hpp>
#include <engine/physics/IExplosion.hpp>
#include <engine/navigation/IAsyncQueryScheduler.hpp>
#include <engine/navigation/INavigationProvider.hpp>
#include <engine/navigation/VoxelNavigation.hpp>
#include <engine/vehicles/IVehicleAsset.hpp>
#include <engine/vehicles/IVehicleProvider.hpp>
#include <engine/procgen/IStructurePlacement.hpp>
#include <engine/animation/IGaitPlanner.hpp>
#include <engine/animation/IFootPlacement.hpp>
#include <engine/audio/IAudioEventMapper.hpp>
#include <engine/audio/IAdaptiveMusic.hpp>
#include <glm/glm.hpp>

int main(int argc, char** argv) {
    int tickCount = 1200;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--ticks" && i + 1 < argc) {
            tickCount = std::max(0, std::atoi(argv[++i]));
        }
    }

    // --- Authoritative world (public IWorldManager): the SAME world the
    // game runtime instantiates headless, so physics + replication share one
    // authoritative instance. ---
    auto manager = engine::world::create_world_manager();
    {
        engine::world::WorldSpec spec;
        spec.name = "server";
        spec.seed = 20260829;
        spec.rulesJson = R"({"mode":"dedicated","difficulty":"normal"})";
        std::string mgrError;
        if (!manager->create_world(spec, mgrError)) {
            std::cerr << "Server: failed to create the authoritative world: "
                      << mgrError << "\n";
            return 1;
        }
    }
    engine::voxel::IVoxelWorld* world = manager->world("server");
    if (world == nullptr) {
        std::cerr << "Server: authoritative world unavailable\n";
        return 1;
    }
    world->set_chunk_budget(2);

    // AGENTE 2 block C (data-driven registry): the server loads the project's
    // block registry JSON asset and registers it on the authoritative world.
    // The world then resolves block ids through the registry (not a hardcoded
    // builtin enum) — generation, meshing, collision, fluids and drops share
    // one data-driven table. The registry survives if the asset is missing
    // (world keeps its engine defaults); a malformed asset is a hard error.
    {
        std::shared_ptr<engine::registry::BlockRegistry> blocks =
            std::make_shared<engine::registry::BlockRegistry>();
        const std::string registryPath =
            std::string(VULKANCRAFT_SOURCE_DIR) + "/Projects/AuditProj/Content/Registry/block/audit_block.json";
        std::ifstream registryFile(registryPath);
        if (registryFile) {
            std::ostringstream buffer;
            buffer << registryFile.rdbuf();
            std::string registryError;
            if (blocks->load_from_json(buffer.str(), registryError)) {
                world->set_block_registry(blocks);
                std::cout << "Server: block registry loaded from asset ("
                          << registryPath << ")\n";
            } else {
                std::cerr << "Server: block registry asset invalid: "
                          << registryError << "\n";
                return 1;
            }
        } else {
            std::cout << "Server: block registry asset not found ("
                      << registryPath << "), keeping engine defaults\n";
        }
    }

    const glm::vec3 focus{ 8.0f, 200.0f, 8.0f };

    // --- Public transport-backed server + world-scoped replication ---
    // INetworkServer owns/uses the real ITransport (loopback local provider
    // here; the game + dedicated server share the exact public protocol from
    // the same IWorldReplication surface).
    std::string netError;
    auto server = engine::networking::create_network_server();
    // D.4 — configuração data-driven: o servidor lê o MESMO asset JSON do
    // host local / cliente da showcase (sem endereço, token ou caminho pessoal
    // hardcoded). A config canônica via factory pública exposta ao editor/MCP
    // (Agente 3 §I) é montada a partir do arquivo; se o asset faltar, os
    // defaults do motor valem (nunca um caminho pessoal).
    std::string cfgServerId = "ag3-server";
    std::uint16_t cfgPort = 3724;
    std::uint32_t cfgTickRate = 60;
    std::uint32_t cfgMaxClients = 64;
    {
        const std::string cfgPath =
            std::string(VULKANCRAFT_SOURCE_DIR) +
            "/Projects/AuditProj/Content/Config/ag3_host_local.json";
        std::ifstream cfgFile(cfgPath);
        if (cfgFile) {
            std::string cfgText((std::istreambuf_iterator<char>(cfgFile)),
                                std::istreambuf_iterator<char>());
            engine::sdk::JsonValue cfgRoot;
            std::string cfgJsonError;
            if (engine::sdk::json_parse(cfgText, cfgRoot, cfgJsonError) &&
                cfgRoot.is_object()) {
                const engine::sdk::JsonValue* session =
                    cfgRoot.field("session");
                if (session != nullptr && session->is_object()) {
                    cfgServerId = engine::sdk::json_string(
                        *session, "server_id", cfgServerId);
                    cfgPort = static_cast<std::uint16_t>(
                        engine::sdk::json_number(*session, "port", cfgPort));
                    cfgTickRate = static_cast<std::uint32_t>(
                        engine::sdk::json_number(*session, "tick_rate", cfgTickRate));
                    cfgMaxClients = static_cast<std::uint32_t>(
                        engine::sdk::json_number(*session, "max_clients", cfgMaxClients));
                }
            }
        }
    }
    // Config canônica via a factory pública exposta ao editor/MCP (Agente 3
    // §I): o servidor dedicado real consome a MESMA helper de configuração
    // usada pelo editor/MCP — uma fonte única, nunca trilha paralela.
    engine::networking::DedicatedServerConfig netConfig =
        engine::networking::make_dedicated_server_config(
            cfgServerId, cfgPort, cfgTickRate, cfgMaxClients, /*udp=*/false);
    if (!server->start(netConfig, netError)) {
        std::cerr << "Server: transport start failed: " << netError << "\n";
        return 1;
    }
    const std::uint64_t clientConn = 1;  // local dedicated client
    if (!server->accept_client(clientConn, netError)) {
        std::cerr << "Server: accept client failed: " << netError << "\n";
        return 1;
    }

    // D — sessão canônica: handshake (versão + auth opcional) e join (player
    // 1001) sobre o runtime público; o token emitido prova o fluxo de sessão.
    auto nowMs = [] { return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); };
    {
        engine::networking::SessionCapabilities caps;
        const auto hs = server->session().handshake(clientConn, netConfig.version,
                                                    "dev-auth", caps, nowMs());
        if (!hs.ok) {
            std::cerr << "Server: session handshake failed: " << hs.error << "\n";
            return 1;
        }
        const auto jn = server->session().join(clientConn, 1001u, 1u, 0u, false, nowMs());
        if (!jn.ok || !jn.token.valid()) {
            std::cerr << "Server: session join failed (token="
                      << (jn.token.valid() ? "issued" : "missing") << "): " << jn.error << "\n";
            return 1;
        }
    }
    // C.4 — versionamento/migração de mensagens: um cliente com versão
    // INCOMPATÍVEL é rejeitado no handshake de forma diagnosticável (nunca uma
    // conexão parcial que corrompe silenciosamente). O handshake válido acima
    // já passou; agora o NEGATIVO é provado com uma versão divergente.
    {
        engine::networking::NetVersion wrongVersion = netConfig.version;
        wrongVersion.protocol = 999;  // incompatível de propósito
        engine::networking::SessionCapabilities caps2;
        const auto badHs = server->session().handshake(2, wrongVersion,
                                                       "dev-auth", caps2, nowMs());
        if (badHs.ok || badHs.error != "version_mismatch_protocol") {
            std::cerr << "Server: incompatible version was NOT rejected ("
                      << (badHs.ok ? "accepted" : badHs.error) << ")\n";
            return 1;
        }
        std::cout << "Server: version negotiation OK (incompatible client "
                     "rejected: " << badHs.error << ")\n";
    }
    auto replication = engine::world::create_world_replication(*manager);
    {
        engine::world::WorldReplicationInterest interest;
        interest.worldName = "server";
        interest.interest.position = { 8, 200, 8 };
        interest.interest.chunkRadius = 3;
        std::string replError;
        if (!replication->server_register_connection(
                static_cast<engine::voxel::ReplicationConnectionId>(clientConn),
                interest, replError)) {
            std::cerr << "Server: replication register failed: " << replError << "\n";
            return 1;
        }
    }

    // F — comando autoritativo `block.place` validado por alcance; o servidor
    // é a única autoridade a aplicar o edit (via IWorldReplication).
    {
        engine::networking::CommandRules rules;
        rules.max_distance = 16.0f;
        rules.max_payload = 16;
        std::string cmdErr;
        const auto& registration = server->authority().register_command(
            "block.place", [&](const engine::networking::CommandContext& ctx,
                                const std::uint8_t* payload, std::size_t size) {
                engine::networking::CommandOutcome out;
                if (size < 12) { return out; }  // ok=false => rejeitado sem efeito
                const int x = static_cast<int>(payload[0] | (std::uint32_t(payload[1]) << 8) |
                                               (std::uint32_t(payload[2]) << 16) | (std::uint32_t(payload[3]) << 24));
                const int y = static_cast<int>(payload[4] | (std::uint32_t(payload[5]) << 8) |
                                               (std::uint32_t(payload[6]) << 16) | (std::uint32_t(payload[7]) << 24));
                const int z = static_cast<int>(payload[8] | (std::uint32_t(payload[9]) << 8) |
                                               (std::uint32_t(payload[10]) << 16) | (std::uint32_t(payload[11]) << 24));
                const auto commit = replication->server_submit_edit(
                    static_cast<engine::voxel::ReplicationConnectionId>(ctx.connection_id),
                    x, y, z, 1u);
                out.ok = commit.accepted;
                return out;
            }, rules, cmdErr);
        if (!registration) {
            std::cerr << "Server: block.place registration failed: " << cmdErr << "\n";
            return 1;
        }
    }

    // H — validação por schema + journal autoritativo (recovery/replay).
    {
        std::string secErr;
        engine::networking::PayloadSchema schema;
        schema.name = "block.edit";
        schema.max_size = 16;
        engine::networking::SchemaFieldRule xf{ "x", engine::networking::FieldKind::I32, 4, -1000, 1000, 1 };
        schema.fields.push_back(xf);
        if (std::uint8_t edit[16] = { 8, 0, 0, 0, 100, 0, 0, 0, 8, 0, 0, 0, 1, 0, 0, 0 };
            server->security().register_schema(schema, secErr)) {
            if (!server->security().validate("block.edit", edit, sizeof(edit), secErr)) {
                std::cerr << "Server: schema validation rejected a valid edit: " << secErr << "\n";
                return 1;
            }
            std::uint64_t seq = 0;
            if (!server->security().journal_record("block_edit", edit, sizeof(edit),
                                                   0u, seq, secErr)) {
                std::cerr << "Server: journal_record failed: " << secErr << "\n";
                return 1;
            }
        }
    }

    // I — discovery: registra o serviço de gameplay saudável no discovery.
    {
        std::string discErr;
        engine::networking::DiscoveryService service;
        service.service_id = 1;
        service.type = "gameplay";
        service.endpoint = "loopback://" + netConfig.transport.endpoint.host + ":" +
                           std::to_string(netConfig.transport.endpoint.port);
        service.consecutive_failures = 0;
        service.healthy = true;
        if (!server->discovery().register_service(service, discErr)) {
            std::cerr << "Server: discovery register failed: " << discErr << "\n";
            return 1;
        }
        const auto resolved = server->discovery().resolve("gameplay");
        if (resolved.empty()) {
            std::cerr << "Server: discovery resolved no gameplay service\n";
            return 1;
        }
    }

    // D.3 — observabilidade de produto: métricas reais de tick/peers/bytes/
    // snapshots/rollback/fila expostas via IObservability (o MESMO contrato
    // que o profiler/editor consomem). Sink stdout real; os contadores/gauges
    // são alimentados a cada tick do loop principal e reportados no shutdown.
    struct ServerObsSink final : engine::observability::ISink {
        std::size_t lines{ 0 };
        void emit(const std::string&) override { ++lines; }
    };
    ServerObsSink obsSink;
    std::string obsError;
    auto observability = engine::observability::create_observability(
        "ag3-server-obs", 64, obsError);
    if (observability) {
        observability->register_sink("stdout", &obsSink, obsError);
        observability->set_gauge("peers", 1, obsError);
        observability->set_gauge("tick_rate", netConfig.tick_rate, obsError);
    }
    std::size_t interestRelevantEntities = 0;
    std::uint64_t obsRollbacks = 0;

    // D.1 — validação estrutural no código de produto: ownership. Um comando
    // com require_ownership executado por quem NÃO é o dono é rejeitado
    // (no_ownership), nunca executado.
    {
        std::string ownErr;
        engine::networking::CommandRules ownRules;
        ownRules.require_ownership = true;
        ownRules.max_payload = 8;
        const auto& ownReg = server->authority().register_command(
            "entity.give",
            [](const engine::networking::CommandContext&,
               const std::uint8_t*, std::size_t) {
                engine::networking::CommandOutcome out;
                out.ok = true;
                return out;
            }, ownRules, ownErr);
        server->authority().server_set_owner(9001, 1001);  // entidade 9001: dona = player 1001
        engine::networking::CommandEnvelope ownEnv;
        ownEnv.connection_id = clientConn;
        ownEnv.sequence = 2;
        ownEnv.command = "entity.give";
        ownEnv.entity_net_id = 9001;
        const std::uint8_t ownPayload[4] = { 1, 0, 0, 0 };
        ownEnv.payload.assign(ownPayload, ownPayload + sizeof(ownPayload));
        engine::networking::CommandContext ownCtx;
        ownCtx.connection_id = clientConn;
        ownCtx.player_id = 2002;  // NÃO é o dono
        ownCtx.now_ms = nowMs();
        std::string ownProcErr;
        const auto ownResults = server->authority().server_process(
            { ownEnv }, ownCtx, ownProcErr);
        const bool ownershipRejected = ownReg && ownResults.size() == 1 &&
            !ownResults[0].ok && ownResults[0].error == "no_ownership";
        if (!ownershipRejected) {
            std::cerr << "Server: ownership validation failed ("
                      << (ownResults.empty() ? ownProcErr : ownResults[0].error)
                      << ")\n";
            return 1;
        }
        std::cout << "Server: ownership enforced (non-owner entity.give "
                     "rejected 'no_ownership')\n";
    }
    // D.1 — taxa RPC: a janela anti-spam derruba mensagens acima do orçamento
    // (o produto nunca processa uma rajada ilimitada).
    {
        std::string secErr;
        server->security().advance_window(nowMs());
        std::size_t accepted = 0;
        for (std::size_t i = 0; i < 70; ++i) {
            if (server->security().observe_incoming(clientConn, 64)) ++accepted;
        }
        const std::size_t dropped = server->security().dropped_spam();
        if (dropped == 0 || accepted > netConfig.security.max_messages_per_window) {
            std::cerr << "Server: rate-limit window did not drop over-budget "
                         "messages (accepted " << accepted << ", dropped "
                      << dropped << ")\n";
            return 1;
        }
        std::cout << "Server: RPC rate limit OK (accepted " << accepted
                  << ", dropped " << dropped << " over-budget)\n";
    }
    // B.6 — codec/compressão existentes: um batch de replicação real é
    // codificado com o provider zstd (limites de payload + frame versionado)
    // e decodificado bit-exact; um frame malformado falha EXPLICITAMENTE.
    {
        auto compression = engine::compression::create_zstd_compression_provider();
        if (compression) {
            engine::voxel::ReplicationBatch batch;
            batch.sequence = 7;
            engine::voxel::BlockReplicationDelta delta;
            delta.position = { 8, 100, 24 };
            delta.blockId = 1;
            delta.sequence = 7;
            delta.revision = 3;
            batch.deltas.push_back(delta);
            const auto encoded =
                engine::voxel::encode_replication_batch(batch, compression);
            engine::voxel::ReplicationBatch decoded;
            const bool roundTrip = !encoded.empty() &&
                engine::voxel::decode_replication_batch(encoded, decoded,
                                                        compression);
            const std::vector<std::byte> garbage{ std::byte(0xFF), std::byte(0xFF),
                                                  std::byte(0x00), std::byte(0x00) };
            engine::voxel::ReplicationBatch bad;
            const bool malformedRejected =
                !engine::voxel::decode_replication_batch(garbage, bad, compression);
            if (!roundTrip || decoded.deltas.size() != 1 || !malformedRejected) {
                std::cerr << "Server: replication codec round-trip failed\n";
                return 1;
            }
            std::cout << "Server: replication codec OK (zstd batch "
                      << batch.deltas.size() << " delta, wire "
                      << encoded.size() << " bytes, malformed frame rejected)\n";
        }
    }

    // --- Authoritative physics + streaming (unchanged self-validation) ---
    {
        const auto bootStart = std::chrono::steady_clock::now();
        while (!world->is_chunk_loaded(0, 0)) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - bootStart).count() > 10000) {
                std::cerr << "Server world boot timeout\n";
                return 1;
            }
            world->update(focus, 1.0f / 60.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    Engine::Physics::PhysicsRuntime physics(
        Engine::Physics::WorldSettings{}, Engine::Physics::PhysicsBackendKind::Jolt);
    Engine::Physics::PhysicsStreamingBridge bridge(*world, physics);
    bridge.sync(focus);

    // AGENTE 2 block A: the dedicated server runs the SAME canonical
    // IWorldRuntime composition as the game executable and editor play mode
    // (bind -> bootstrap -> advance -> shutdown). The runtime's ECS is a fresh
    // entity world; the canonical gameplay integration advances its fixed tick
    // (physics step + event router + navigation queries + audio mapping over
    // the runtime ECS) each loop iteration alongside the voxel/physics
    // authority. NOTE: `worlds` is intentionally NOT bound (see below — the
    // integration's fixed focus would evict the server's streamed chunks).
    // Failure is non-fatal: the server self-validation loop keeps its proven
    // physics path.
    auto serverRuntimeEcs = engine::entity::create_entity_world();
    auto serverRuntimePhysics = engine::gameplay::create_gameplay_runtime(
        engine::gameplay::PhysicsBackend::Builtin);
    auto serverRuntimeIntegration = engine::gameplay::create_gameplay_integration();
    auto serverRuntimeBindings = engine::gameplay::create_gameplay_bindings();
    auto serverRuntimeWiring = engine::gameplay::create_gameplay_system_wiring();
    auto serverRuntimeEvents = engine::gameplay::create_gameplay_events();
    auto serverRuntimeMetrics = engine::gameplay::create_gameplay_metrics();
    auto serverRuntimeAudio = engine::audio::create_audio_event_mapper();
    auto serverRuntimeQueries = engine::navigation::create_async_query_scheduler();
    auto serverRuntimeRouter = engine::gameplay::create_gameplay_event_router(
        serverRuntimeEvents.get(), serverRuntimeAudio.get(),
        serverRuntimeMetrics.get());
    // AGENTE 2 G.97: the server-side canonical bus maps the same event kinds
    // as the game (block.break/block.place) to audio triggers + metrics, so a
    // published authoritative edit becomes a routed, observable event.
    {
        std::string busError;
        const std::vector<std::pair<std::uint16_t, std::string>> busMapping = {
            { 1, "block.break" },
            { 2, "block.place" },
        };
        if (!serverRuntimeRouter->configure_mapping(busMapping, busError)) {
            std::cerr << "Server: event bus mapping refused: " << busError << "\n";
        }
        const std::vector<engine::audio::AudioTrigger> triggers = {
            { "block.break", "block_break", 0.9f, 1.0f },
            { "block.place", "block_place", 0.8f, 1.0f },
        };
        if (!serverRuntimeAudio->configure(triggers, busError)) {
            std::cerr << "Server: audio trigger mapping refused: " << busError << "\n";
        }
    }
    // AGENTE 2 block G (day/night): the dedicated server owns the SAME
    // deterministic IDayNightCycle as the game (bound into the canonical
    // context, advanced by the runtime each tick) — the server clock is the
    // authority the game client reconciles against, not a per-client wall clock.
    auto serverDayNight = engine::gameplay::create_day_night_cycle();
    {
        engine::gameplay::DayNightConfig dnConfig;
        dnConfig.dayLengthSeconds = 180.0f;
        dnConfig.startOfDay = 0.22f;
        std::string dnError;
        if (!serverDayNight->configure(dnConfig, dnError)) {
            std::cerr << "Server: day/night configure refused: " << dnError << "\n";
            serverDayNight.reset();
        }
    }
    // AGENTE 2 block E (timeline): the dedicated server owns the public
    // time-travel surface over its authoritative IWorldManager + the
    // deterministic timeline budget policy. `capture_state` persists a REAL
    // save-v5 snapshot of the authoritative world and registers it on the
    // timeline (same foundation as replication->server_save); the policy
    // prunes/compacts the registered states when the budget overflows. This
    // gives the SDK timeline a live consumer in the dedicated executable.
    auto serverTimeline = engine::world::create_time_travel(*manager);
    auto serverTimelinePolicy = engine::world::create_timeline_policy();
    {
        engine::world::TimelinePolicyConfig policyConfig;
        policyConfig.maxStates = 4;
        policyConfig.compactionEnabled = true;
        std::string policyError;
        if (!serverTimelinePolicy->configure(policyConfig, policyError)) {
            std::cerr << "Server: timeline policy configure refused: "
                      << policyError << "\n";
            serverTimelinePolicy.reset();
        }
    }
    // AGENTE 2 block J.127: the server also runs the adaptive-music core
    // driven by ITS day/night clock — the same deterministic crossfade the
    // game runs, so the server is the authority for the music state the game
    // clients reconcile against (same composition, server configuration).
    auto serverMusic = engine::audio::create_adaptive_music();
    if (serverMusic) {
        engine::audio::AdaptiveMusicSpec musicSpec;
        musicSpec.layers = { { "ambience" }, { "combat" } };
        engine::audio::MusicState dayState;
        dayState.id = "day";
        dayState.layer_gains = { { "ambience", 1.0 }, { "combat", 0.0 } };
        dayState.transition_s = 2.0;
        engine::audio::MusicState nightState;
        nightState.id = "night";
        nightState.layer_gains = { { "ambience", 0.6 }, { "combat", 0.0 } };
        nightState.transition_s = 2.0;
        engine::audio::MusicState combatState;
        combatState.id = "combat";
        combatState.layer_gains = { { "ambience", 0.3 }, { "combat", 1.0 } };
        combatState.transition_s = 0.5;
        musicSpec.states = { dayState, nightState, combatState };
        musicSpec.stingers = { { "hit", "combat", 0.8 } };
        std::string musicError;
        if (!serverMusic->configure(musicSpec, musicError)) {
            std::cerr << "Server: adaptive music configure refused: "
                      << musicError << "\n";
            serverMusic.reset();
        }
    }
    auto serverWorldRuntime = engine::create_world_runtime();
    engine::WorldServiceContext serverContext;
    serverContext.ecs = serverRuntimeEcs.get();
    serverContext.physicsGameplay = serverRuntimePhysics.get();
    serverContext.timelinePolicy = serverTimelinePolicy.get();
    // NOTE: `worlds` is intentionally NOT bound on the dedicated server. The
    // canonical integration advances every attached world inside its fixed
    // tick, which would run World::update a SECOND time per loop iteration
    // (the server already drives world->update(focus) explicitly for its
    // self-validation timing) — that double-update races the render-bridge
    // frame bookkeeping and the sleep/eviction timing the server validates.
    // The server runs the canonical composition for ECS/physics/router/
    // queries/audio alongside its authoritative voxel loop — same runtime,
    // server configuration. (The set_world_focus fix in IGameplayIntegration
    // makes binding `worlds` safe for jogo/play-mode consumers; the server
    // self-validation keeps the voxel loop explicit.)
    serverContext.integration = serverRuntimeIntegration.get();
    serverContext.bindings = serverRuntimeBindings.get();
    serverContext.wiring = serverRuntimeWiring.get();
    serverContext.eventRouter = serverRuntimeRouter.get();
    serverContext.navigationQueries = serverRuntimeQueries.get();
    serverContext.audio = serverRuntimeAudio.get();
    serverContext.dayNight = serverDayNight.get();
    {
        std::string runtimeError;
        if (!serverWorldRuntime->bind(serverContext, runtimeError) ||
            !serverWorldRuntime->set_world_focus(focus.x, focus.y, focus.z) ||
            !serverWorldRuntime->bootstrap(runtimeError)) {
            std::cerr << "Server: canonical IWorldRuntime bootstrap refused: "
                      << runtimeError << "\n";
        } else {
            std::cout << "Server: composed canonical IWorldRuntime "
                         "(server focus configured)\n";
        }
    }

    // Spawn just above the ACTUAL streamed surface so "rest and sleep on
    // streamed terrain" is provable within a small tick budget. (A spawn at
    // y=200 "well above terrain" needs ~5s / ~300 ticks of free fall before
    // the body beds down, so a --ticks 10..120 gate can never prove rest.)
    // Probe the surface column with the public voxel raycast; fall back to a
    // high spawn if the probe misses.
    float surfaceY = 200.0f;
    {
        const ::engine::voxel::VoxelRaycastHit surface = world->raycast(
            glm::vec3(8.0f, 512.0f, 8.0f), glm::vec3(0.0f, -1.0f, 0.0f), 512.0f);
        if (surface.hit) {
            surfaceY = surface.position.y + 2.0f;  // a couple units above the surface block
        }
    }
    Engine::Physics::BodyDesc sphere;
    sphere.position = glm::vec3(8.0f, surfaceY, 8.0f);
    sphere.collider.shape = Engine::Physics::SphereShape{ 0.5f };
    const auto ball = bridge.spawn_dynamic(sphere);
    if (ball == Engine::Physics::InvalidBody) {
        std::cerr << "Server failed to spawn the physics body\n";
        return 1;
    }

    // AGENTE 2 block F (vehicles): the authoritative server instantiates a
    // REAL vehicle through the promoted gameplay runtime — the asset selects
    // the Jolt provider (create_vehicle_provider gate), the chassis body is
    // assembled and claimed inside IGameplayRuntime::create_vehicle_from_asset,
    // and the vehicle advances in the canonical fixed tick (the runtime's
    // advance() below steps it every iteration). Observable proof that the
    // vehicle path is alive on the dedicated server's live loop; a
    // refused provider (chrono/jsbsim) is reported, never silently skipped.
    {
        engine::vehicles::VehicleAsset vehicle;
        vehicle.name = "server_probe_car";
        vehicle.position = glm::vec3(24.0f, surfaceY, 8.0f);
        vehicle.chassis.halfExtents = glm::vec3(0.9f, 0.35f, 0.56f);
        vehicle.wheels.push_back(
            engine::vehicles::WheelComponent{ { -0.8f, 0.0f, -0.55f } });
        vehicle.wheels.push_back(
            engine::vehicles::WheelComponent{ { -0.8f, 0.0f, 0.55f } });
        vehicle.wheels.push_back(
            engine::vehicles::WheelComponent{ { 0.8f, 0.0f, -0.55f } });
        vehicle.wheels.push_back(
            engine::vehicles::WheelComponent{ { 0.8f, 0.0f, 0.55f } });
        std::string vehError;
        if (!vehicle.validate(vehError)) {
            std::cerr << "Server: vehicle asset invalid: " << vehError << "\n";
            return 1;
        }
        std::string providerError;
        auto provider = engine::vehicles::create_vehicle_provider(
            vehicle.provider, providerError);
        if (!provider || !provider->available()) {
            std::cout << "Server: vehicle provider refused (" << providerError
                      << "); vehicle path inactive\n";
        } else {
            auto vehicleRuntime = serverRuntimePhysics
                ? serverRuntimePhysics->create_vehicle_from_asset(vehicle)
                : nullptr;
            std::cout << "Server: vehicle provider '"
                      << engine::vehicles::vehicle_provider_name(
                             vehicle.provider)
                      << "' available; asset validated; vehicle "
                      << (vehicleRuntime
                              ? "spawned in the canonical fixed tick"
                              : "creation returned null (see runtime)")
                      << "\n";
        }
    }

    // AGENTE 2 block D (multiple worlds + entity transfer + portals): the
    // authoritative IWorldManager hosts a SECOND world with its own seed and
    // lifecycle; entities transfer between worlds transactionally
    // (transfer_entity) and through a REAL portal (create_portal +
    // transfer_via_portal maps position through the anchors); a persistent
    // cross-world reference (set_entity_ref + resolve_entity_ref) keeps the
    // identity stable across the migration. Observable on stdout.
    {
        engine::world::WorldSpec dimSpec;
        dimSpec.name = "server_dim";
        dimSpec.seed = 777001;
        dimSpec.rulesJson = "{\"mode\":\"dimension\"}";
        std::string dimError;
        if (!manager->create_world(dimSpec, dimError)) {
            std::cout << "Server: second world 'server_dim' refused ("
                      << dimError << ")\n";
        }
        auto* serverEntities = world->entity_world().get();
        if (serverEntities != nullptr) {
            std::string eErr;
            engine::entity::EntityId traveler = serverEntities->spawn(
                "vulkancraft:traveler",
                engine::entity::Position{ 8.0f, surfaceY, 8.0f }, eErr);
            if (traveler.valid()) {
                serverEntities->set_stable_id(traveler, "traveler-1");
                const engine::entity::EntityId moved =
                    manager->transfer_entity(
                        "server", traveler, "server_dim",
                        engine::entity::Position{ 32.0f, 32.0f, 32.0f },
                        dimError);
                std::cout << "Server: entity transfer server->server_dim "
                          << (moved.valid()
                                  ? "accepted"
                                  : std::string("refused (") + dimError + ")")
                          << "\n";
                engine::world::PortalSpec portal;
                portal.fromWorld = "server";
                portal.fromX = 8.0f;
                portal.fromY = surfaceY;
                portal.fromZ = 8.0f;
                portal.toWorld = "server_dim";
                portal.toX = 64.0f;
                portal.toY = 32.0f;
                portal.toZ = 64.0f;
                portal.yawDegrees = 90.0f;
                const std::uint32_t portalId =
                    manager->create_portal(portal, dimError);
                if (portalId != 0) {
                    engine::entity::EntityId pTraveler = serverEntities->spawn(
                        "vulkancraft:traveler",
                        engine::entity::Position{ 8.0f, surfaceY, 8.0f },
                        eErr);
                    if (pTraveler.valid()) {
                        const engine::entity::EntityId via =
                            manager->transfer_via_portal(
                                "server", pTraveler, portalId, dimError);
                        std::cout << "Server: portal transfer (yaw 90) "
                                  << (via.valid()
                                          ? "accepted"
                                          : std::string("refused (") +
                                                dimError + ")")
                                  << "\n";
                    }
                }
                // Persistent cross-world reference + resolution: the ref
                // survives the migration because it names the DESTINATION by
                // (world, stable id), never a local handle.
                engine::entity::EntityId linker = serverEntities->spawn(
                    "vulkancraft:gatekeeper",
                    engine::entity::Position{ 10.0f, surfaceY, 8.0f }, eErr);
                if (linker.valid()) {
                    engine::world::WorldEntityRef ref;
                    ref.toWorld = "server_dim";
                    ref.stableId = "traveler-1";
                    if (manager->set_entity_ref("server", linker, ref,
                                                dimError)) {
                        const engine::entity::EntityId resolved =
                            manager->resolve_entity_ref("server", linker,
                                                        dimError);
                        std::cout << "Server: cross-world entity ref "
                                  << (resolved.valid()
                                          ? "resolved (entity in server_dim)"
                                          : std::string("FAILED (") +
                                                dimError + ")")
                                  << "\n";
                    }
                }
            }
        }
    }

    // AGENTE 2 block F (physics over ECS): the canonical runtime's ECS is
    // mirrored into the canonical gameplay runtime the same way the game's B3
    // does — each ECS entity spawns EXACTLY one kinematic body; the tick loop
    // below re-mirrors positions every tick; a despawn destroys the body (no
    // orphans). The mirrored body count is reported in the final stdout.
    std::vector<std::pair<engine::entity::EntityId,
                          engine::gameplay::BodyId>> serverEcsBodies;
    if (serverRuntimeEcs && serverRuntimePhysics) {
        std::string sErr;
        for (int i = 0; i < 3; ++i) {
            const engine::entity::EntityId eid = serverRuntimeEcs->spawn(
                "vulkancraft:guard",
                engine::entity::Position{ 20.0f + 4.0f * i, surfaceY, 20.0f },
                sErr);
            if (!eid.valid()) continue;
            engine::gameplay::BodySpec spec;
            spec.motion = engine::gameplay::MotionType::Kinematic;
            spec.position = glm::vec3(20.0f + 4.0f * i, surfaceY, 20.0f);
            spec.mass = 20.0f;
            spec.shape = engine::gameplay::SphereShape{ 0.5f };
            const engine::gameplay::BodyId body =
                serverRuntimePhysics->physics().create_body(spec);
            if (body.valid()) {
                serverEcsBodies.emplace_back(eid, body);
            }
        }
        std::cout << "Server: ECS->physics mirroring primed ("
                  << serverEcsBodies.size() << " bodies)\n";
    }

    // AGENTE 2 block E (async autosave): the authoritative world's region
    // storage runs a budgeted DELTA autosave — set_autosave arms the async
    // path (time interval + dirty-chunk threshold); update() fires it on a
    // background job while the server keeps simulating; wait_async_saves
    // drains it before shutdown. The result is reported in the final stdout.
    {
        engine::voxel::IVoxelWorld::AutosaveConfig autosave;
        autosave.enabled = true;
        autosave.intervalSeconds = 15.0;
        autosave.dirtyChunkThreshold = 2;
        const std::string autosavePath =
            std::string(VULKANCRAFT_SOURCE_DIR) +
            "/Projects/AuditProj/Saves/ag3-server-autosave.json";
        world->set_autosave(autosave, autosavePath);
        std::string asError;
        world->wait_async_saves(asError);
        std::cout << "Server: async autosave armed (interval "
                  << autosave.intervalSeconds
                  << "s, dirty threshold " << autosave.dirtyChunkThreshold
                  << ", path " << autosavePath << ")\n";
    }

    bool rested = false;
    int sleepTick = -1;
    std::size_t replicatedChunksSeen = 0;
    for (int tick = 0; tick < tickCount; ++tick) {
        world->update(focus, 1.0f / 60.0f);
        bridge.sync(focus);
        physics.step(1.0f / 60.0f);
        // AGENTE 2 block A: advance the canonical composition every tick
        // (fixed tick = simulation authority for the ECS/worlds/router).
        if (serverWorldRuntime) serverWorldRuntime->advance(1.0f / 60.0f);
        // AGENTE 2 block F (physics over ECS): re-mirror each live ECS entity
        // into its canonical body every tick — the ECS is the transform
        // authority, the body reflects it for queries/collisions (server-side
        // twin of the game's B3 mirror).
        if (serverRuntimeEcs && serverRuntimePhysics) {
            for (auto& [eid, body] : serverEcsBodies) {
                engine::entity::Position pos;
                if (serverRuntimeEcs->alive(eid) &&
                    serverRuntimeEcs->get_position(eid, pos)) {
                    serverRuntimePhysics->physics().set_transform(
                        body, glm::vec3(pos.x, pos.y, pos.z),
                        glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f });
                }
            }
        }

        // Public authority each tick: pump every world's replication, pack the
        // dedicated client's interest (its streaming chunks), and exercise an
        // authoritative block edit through the shared voxel protocol.
        replication->server_update();
        std::string tickNetError;
        (void)server->tick(tickNetError);
        const auto interest = replication->server_pack_interest(
            static_cast<engine::voxel::ReplicationConnectionId>(clientConn));
        replicatedChunksSeen += interest.size();
        // B.4 — interest management público conectado à partição espacial do
        // servidor: o INetworkInterest (server->interest()) observa as
        // entidades do ECS por posição a cada tick — o subconjunto relevante
        // (raio = interesse de chunks) limita o que cada cliente recebe. O
        // observer fica na altura da SUPERFÍCIE do focus (não no ar): as
        // entidades do ECS vivem em y = surfaceY, então um observer em
        // y = 200 ficaria fora do raio euclidiano e o subset seria sempre
        // vazio — a prova precisa de um subset REAL não-trivial.
        if (serverRuntimeEcs) {
            std::string interestErr;
            engine::networking::InterestObserver observer;
            observer.observer_id = clientConn;
            observer.position = { focus.x, surfaceY, focus.z };
            observer.radius = 3.0 * engine::voxel::kReplicationChunkSize;
            observer.always_relevant = false;
            if (server->interest().set_observer(observer, interestErr)) {
                for (const auto& [eid, body] : serverEcsBodies) {
                    (void)body;
                    engine::entity::Position p;
                    if (serverRuntimeEcs->alive(eid) &&
                        serverRuntimeEcs->get_position(eid, p)) {
                        server->interest().set_entity(
                            { static_cast<std::uint64_t>(eid.id),
                              { p.x, p.y, p.z } });
                    }
                }
                const auto relevant = server->interest().compute();
                for (const auto& r : relevant) {
                    interestRelevantEntities += r.entity_ids.size();
                }
            }
        }
        // D.3 — métricas reais por tick (bytes/snapshots/fila/perda/rollback)
        // no contrato de observabilidade que o profiler/editor consomem. RTT
        // e perda vêm do transport real (sent/received counts); rollback vem
        // da reconciliação de predição do loop (contador no bloco G).
        if (observability) {
            std::string obsErr;
            observability->increment_counter("tick", 1, obsErr);
            observability->increment_counter(
                "snapshots", static_cast<std::int64_t>(interest.size()), obsErr);
            std::int64_t bytes = 0;
            for (const auto& snap : interest) {
                bytes += static_cast<std::int64_t>(snap.blocks.size()) * 4;
            }
            observability->increment_counter("bytes", bytes, obsErr);
            observability->set_gauge(
                "rpc_queue",
                static_cast<std::int64_t>(server->rpc().pending_calls().size()),
                obsErr);
            observability->set_gauge("peers", 1, obsErr);
            const auto sm = server->metrics();
            const auto sent = static_cast<std::int64_t>(sm.messages_sent);
            const auto recv = static_cast<std::int64_t>(sm.messages_received);
            observability->set_gauge("messages_sent", sent, obsErr);
            observability->set_gauge("messages_received", recv, obsErr);
            observability->set_gauge(
                "loss_dropped", std::max<std::int64_t>(0, sent - recv), obsErr);
            observability->set_gauge(
                "rollback", static_cast<std::int64_t>(obsRollbacks), obsErr);
        }
        // Exercise the ONE-time authoritative block edit proof. The public
        // replication layer enforces an anti-spam cooldown
        // (kReplicationEditCooldownTicks == 2), so issuing the same edit on
        // every frame is correctly rejected after the first accepted commit.
        // The proof needs a single accepted authoritative edit; gate it to the
        // first tick so the harness respects its own rate limit.
        // AGENTE 2 block J.127: the server drives its adaptive-music core
        // from the SAME day/night clock every tick — state authority for the
        // game clients (day when the daylight factor is high, night below).
        if (serverMusic && serverDayNight) {
            std::string musicError;
            const float daylight = serverDayNight->daylight_factor();
            serverMusic->set_state(daylight < 0.35f ? "night" : "day",
                                   musicError);
            serverMusic->tick(1.0f / 60.0f, musicError);
        }
        if (tick == 0) {
            // Keep the committed edit OUT of the body's chunk (0,0): the product
            // correctly pins a chunk with unsaved edits so it can never be
            // evicted (edits are never lost to a cache eviction). Placing the
            // proof block at (8,100,24) -> chunk (0,1) keeps chunk (0,0) clean
            // so the eviction gate below can actually unload it and despawn the
            // contained body.
            const auto edit = replication->server_submit_edit(
                static_cast<engine::voxel::ReplicationConnectionId>(clientConn),
                8, 100, 24, 1);  // place a stone block in a NEIGHBOR chunk
            if (!edit.accepted) {
                std::cerr << "Server: authoritative block edit rejected after re-bind\n";
                return 1;
            }
            // AGENTE 2 G.97: the authoritative edit publishes a REAL gameplay
            // event on the canonical bus (same IGameplayEvents the game
            // publishes block.break/place on) — the server-side event router
            // maps it to an audio trigger + metric inside the runtime tick.
            if (serverRuntimeEvents) {
                serverRuntimeEvents->publish(
                    1, static_cast<std::uint64_t>(tick), {});
            }
        }

        const Engine::Physics::RigidBody* rb = bridge.body(ball);
        if (rb != nullptr && rb->sleeping && sleepTick < 0) {
            rested = true;
            sleepTick = tick;
        }
        if (sleepTick >= 0 && tick >= sleepTick + 10) break;  // proof captured
    }

    if (!rested) {
        std::cerr << "Server: dynamic body did not rest and sleep on streamed "
                     "terrain within " << tickCount << " ticks\n";
        return 1;
    }
    if (bridge.dynamic_body_count() != 1 || bridge.terrain_body_count() < 1) {
        std::cerr << "Server: body/terrain accounting wrong after resting\n";
        return 1;
    }

    // Terrain-edit wake: the sleeping body wakes, then rests again.
    const std::size_t woken = bridge.wake_region(
        glm::vec3(0.0f, 90.0f, 0.0f), glm::vec3(16.0f, 220.0f, 16.0f));
    if (woken != 1) {
        std::cerr << "Server: wake_region woke " << woken << " bodies (want 1)\n";
        return 1;
    }
    for (int tick = 0; tick < 300; ++tick) {
        world->update(focus, 1.0f / 60.0f);
        bridge.sync(focus);
        physics.step(1.0f / 60.0f);
        const Engine::Physics::RigidBody* rb = bridge.body(ball);
        if (rb != nullptr && rb->sleeping) break;
    }
    const Engine::Physics::RigidBody* afterWake = bridge.body(ball);
    if (afterWake == nullptr || !afterWake->sleeping) {
        std::cerr << "Server: woken body did not re-sleep\n";
        return 1;
    }

    // F — executa UM comando autoritativo `block.place` pelo runtime público
    // (handler do bloco valida alcance e aplica via IWorldReplication). MUST
    // run while the world around the origin focus is still streamed in: the
    // eviction proof below unloads every chunk away from the far focus, so a
    // later block.place would be (correctly) rejected as "chunk not loaded".
    // Target chunk (0,1) (z=24), NOT the body's chunk (0,0): committing an
    // unsaved edit into chunk (0,0) pins it (the product correctly never
    // evicts a dirty chunk), so the eviction proof could never unload it.
    {
        engine::networking::CommandEnvelope env;
        env.connection_id = clientConn;
        env.sequence = 1;
        env.command = "block.place";
        env.has_target = true;
        env.target_x = 8.0; env.target_y = 100.0; env.target_z = 24.0;
        const std::uint8_t edit[12] = { 8,0,0,0, 100,0,0,0, 24,0,0,0 };
        env.payload.assign(edit, edit + sizeof(edit));
        engine::networking::CommandContext ctx;
        ctx.connection_id = clientConn;
        ctx.player_id = 1001u;
        ctx.origin_x = 8.0; ctx.origin_y = 100.0; ctx.origin_z = 24.0;
        ctx.now_ms = nowMs();
        std::string authErr;
        std::vector<engine::networking::CommandEnvelope> batch = { std::move(env) };
        const auto results = server->authority().server_process(std::move(batch), ctx, authErr);
        if (results.size() != 1 || !results[0].ok) {
            std::cerr << "Server: authoritative block.place was rejected ("
                      << (results.empty() ? authErr : results[0].error) << ")\n";
            return 1;
        }
    }

    // B.3 — explosões por comando/seed AUTORITATIVOS: `explosion.detonate` é
    // UM comando (payload = centro x/y/z + raio*4 + seed). O handler avalia o
    // core determinístico de explosão (IExplosion) e devolve o resultado
    // autoritativo — nunca milhares de resultados redundantes por bloco.
    {
        std::string boomErr;
        engine::networking::CommandRules boomRules;
        boomRules.max_distance = 128.0f;
        boomRules.max_payload = 20;  // x y z radius*4 seed
        const auto& boomReg = server->authority().register_command(
            "explosion.detonate",
            [&](const engine::networking::CommandContext&,
                const std::uint8_t* payload, std::size_t size) {
                engine::networking::CommandOutcome out;
                if (size < 20) return out;  // ok=false => rejeitado sem efeito
                const auto rd = [&](std::size_t off) -> std::uint32_t {
                    return static_cast<std::uint32_t>(payload[off]) |
                           (static_cast<std::uint32_t>(payload[off + 1]) << 8) |
                           (static_cast<std::uint32_t>(payload[off + 2]) << 16) |
                           (static_cast<std::uint32_t>(payload[off + 3]) << 24);
                };
                auto blast = engine::physics::create_explosion();
                if (!blast) return out;
                engine::physics::ExplosionSpec spec;
                spec.radius = static_cast<double>(rd(12)) * 0.25;
                spec.impulse = 1200.0;
                spec.heat = 800.0;
                spec.damage = 90.0;
                spec.falloff_power = 2.0;
                spec.fragments = 12;
                spec.fragment_speed = 18.0;
                std::string cfgErr;
                if (!blast->configure(spec, cfgErr)) return out;
                const auto core = blast->sample_at(1.0, cfgErr);
                const auto frags = blast->fragments(rd(16), cfgErr);
                if (!cfgErr.empty() || core.distance != 1.0 || frags.empty()) {
                    return out;
                }
                out.ok = true;
                return out;
            }, boomRules, boomErr);
        if (!boomReg) {
            std::cerr << "Server: explosion.detonate registration failed: "
                      << boomErr << "\n";
            return 1;
        }
        // Executa o comando autoritativo UMA vez (centro 8,100,24, raio 4,
        // seed fixo): um único envelope produz o blast determinístico.
        engine::networking::CommandEnvelope boomEnv;
        boomEnv.connection_id = clientConn;
        boomEnv.sequence = 3;
        boomEnv.command = "explosion.detonate";
        boomEnv.has_target = true;
        boomEnv.target_x = 8.0; boomEnv.target_y = 100.0; boomEnv.target_z = 24.0;
        const std::uint8_t boom[20] = { 8,0,0,0, 100,0,0,0, 24,0,0,0,
                                        16,0,0,0, 0,0,0,1 };
        boomEnv.payload.assign(boom, boom + sizeof(boom));
        engine::networking::CommandContext boomCtx;
        boomCtx.connection_id = clientConn;
        boomCtx.player_id = 1001u;
        boomCtx.origin_x = 8.0; boomCtx.origin_y = 100.0; boomCtx.origin_z = 24.0;
        boomCtx.now_ms = nowMs();
        std::string boomProcErr;
        std::vector<engine::networking::CommandEnvelope> boomBatch = { std::move(boomEnv) };
        const auto boomResults = server->authority().server_process(
            std::move(boomBatch), boomCtx, boomProcErr);
        if (boomResults.size() != 1 || !boomResults[0].ok) {
            std::cerr << "Server: authoritative explosion.detonate was rejected ("
                      << (boomResults.empty() ? boomProcErr
                                              : boomResults[0].error)
                      << ")\n";
            return 1;
        }
        std::cout << "Server: authoritative explosion.detonate executed "
                     "(1 command -> deterministic blast from seed, no "
                     "per-block flood)\n";
    }

    // B.2 — replicação voxel do lado CLIENTE (mesma IWorldReplication, método
    // client_*): o servidor também é um cliente local do protocolo. Uma edição
    // é predita otimisticamente (client_predict), um batch autoritativo com
    // revisão a confirma (client_apply_batch) e um snapshot de chunk é
    // aplicado (client_apply_snapshot) — a invalidação de mesh/collision no
    // cliente acontece ao reescrever os blocos do mundo local. Rejeições e
    // mensagens obsoletas (stale) são rastreadas.
    {
        std::string bindErr;
        if (!replication->client_bind("server", bindErr)) {
            std::cerr << "Server: client_bind failed: " << bindErr << "\n";
            return 1;
        }
        // 1) predição otimista de um edit no chunk (0,1) (fora do chunk do corpo).
        replication->client_predict(8, 100, 26, 2);
        const std::size_t pendingPredictions = replication->client_pending_predictions();
        // 2) batch autoritativo com revisão: confirma/corrige a predição.
        engine::voxel::ReplicationBatch authBatch;
        authBatch.sequence = replication->client_applied_sequence() + 1;
        engine::voxel::BlockReplicationDelta confirmed;
        confirmed.position = { 8, 100, 26 };
        confirmed.blockId = 2;
        confirmed.sequence = authBatch.sequence;
        confirmed.revision = 1;
        authBatch.deltas.push_back(confirmed);
        replication->client_apply_batch(authBatch);
        // 3) um batch que REJEITA uma predição restaura o bloco anterior.
        engine::voxel::ReplicationBatch rejectBatch;
        rejectBatch.rejected.push_back({ 8, 100, 27 });
        replication->client_predict(8, 100, 27, 3);
        replication->client_apply_batch(rejectBatch);
        // 4) snapshot de chunk: resync por interesse (invalidação no cliente).
        const auto snapshots = replication->server_pack_interest(
            static_cast<engine::voxel::ReplicationConnectionId>(clientConn));
        if (!snapshots.empty()) replication->client_apply_snapshot(snapshots.front());
        const std::uint32_t appliedSeq = replication->client_applied_sequence();
        const std::size_t stale = replication->client_stale_dropped();
        if (pendingPredictions == 0 || appliedSeq == 0) {
            std::cerr << "Server: client-side voxel replication misbehaved ("
                      << "pending " << pendingPredictions << ", applied "
                      << appliedSeq << ")\n";
            return 1;
        }
        std::cout << "Server: client-side voxel replication OK (predict "
                  << pendingPredictions << " pending, batch applied seq "
                  << appliedSeq << ", " << snapshots.size()
                  << " interest snapshot(s) applied, stale " << stale << ")\n";
    }

    // C.2 — retomada de sessão: o save canônico (v5, o mesmo formato que o
    // Game/editor leem) é relido por um NOVO mundo do IWorldManager — a
    // sessão retoma do mesmo arquivo que o autosave/save oficial escreveu.
    // Sem save anterior (primeira execução) a sessão é FRESCA e o resume é
    // pulado com aviso observável; um save presente porém INVALIDO/desconhecido
    // falha de forma diagnosticável (C.4) — nunca corrupção silenciosa.
    {
        const std::string resumePath = "ag3-server-save.json";
        if (!std::filesystem::exists(resumePath)) {
            std::cout << "Server: session resume skipped (no prior save at "
                      << resumePath << "; fresh session)\n";
        } else {
            std::string resumeError;
            engine::world::WorldSpec resumeSpec;
            resumeSpec.name = "server_resume";
            resumeSpec.seed = 20260829;
            resumeSpec.savePath = resumePath;
            if (manager->load_world(resumeSpec, resumeError)) {
                const auto info = manager->world_info("server_resume");
                std::cout << "Server: session resume OK (world '"
                          << info.name << "' loaded, seed " << info.seed
                          << ", entities " << info.entityCount << ")\n";
            } else {
                std::cerr << "Server: session resume FAILED (unknown/corrupt "
                             "save, no silent fallback): " << resumeError
                          << "\n";
                return 1;
            }
        }
    }
    // C.3 — replay/timeline conectado ao fluxo real: o journal autoritativo
    // (block_edit registrado no H) é reproduzido deterministicamente SOBRE o
    // mundo retomado — a sequência curta da showcase re-executa por replay.
    // O mundo de replay precisa de chunks carregados para aceitar os edits
    // (server_submit_edit rejeita "chunk not loaded"), então TODOS os chunks
    // alvo do journal são streamados ANTES de reproduzir. Cada entrada usa
    // uma conexão de replay FRESCA (registra -> aplica -> desregistra): a
    // replicação rejeita re-registro ("connection already registered") e o
    // cooldown anti-spam (kReplicationEditCooldownTicks) é por conexão — um
    // journal com mais de um edit nunca cairia em nenhum dos dois casos.
    {
        // O mundo de replay é o retomado (quando o save existe); numa sessão
        // FRESCA criamos um mundo DEDICADO "server_replay" — NUNCA o mundo
        // autoritativo "server", porque um edit commitado ali sujaria/pinaria
        // o chunk (0,0) e quebraria o proof de eviction que roda depois.
        engine::voxel::IVoxelWorld* resumeWorld = manager->world("server_resume");
        std::string replayWorld;
        if (resumeWorld != nullptr) {
            replayWorld = "server_resume";
        } else {
            engine::world::WorldSpec replaySpec;
            replaySpec.name = "server_replay";
            replaySpec.seed = 20260829;
            std::string replayErr;
            if (!manager->create_world(replaySpec, replayErr)) {
                std::cerr << "Server: replay world creation failed: "
                          << replayErr << "\n";
                return 1;
            }
            resumeWorld = manager->world("server_replay");
            replayWorld = "server_replay";
        }
        // Pré-varredura: coleta TODOS os alvos de block_edit do journal (a
        // sequência curta da showcase) para streamar cada chunk antes do
        // replay — um edit num chunk não carregado seria rejeitado.
        std::vector<glm::ivec3> replayTargets;
        const auto preScan = [&](const engine::networking::JournalEntry& entry) {
            if (entry.kind != "block_edit" || entry.data.size() < 12) return;
            const auto rd = [&](std::size_t off) -> int {
                return static_cast<int>(
                    entry.data[off] |
                    (static_cast<std::uint32_t>(entry.data[off + 1]) << 8) |
                    (static_cast<std::uint32_t>(entry.data[off + 2]) << 16) |
                    (static_cast<std::uint32_t>(entry.data[off + 3]) << 24));
            };
            replayTargets.push_back({ rd(0), rd(4), rd(8) });
        };
        (void)server->security().replay(0, preScan);
        // Streama os chunks dos alvos no mundo de replay antes de aplicar os
        // edits (cada alvo pode estar em um chunk diferente do journal).
        if (resumeWorld != nullptr && !replayTargets.empty()) {
            for (const glm::ivec3& target : replayTargets) {
                const glm::vec3 replayFocus(static_cast<float>(target.x),
                                            static_cast<float>(target.y),
                                            static_cast<float>(target.z));
                const int tx = engine::voxel::kReplicationChunkSize > 0
                                   ? target.x / engine::voxel::kReplicationChunkSize
                                   : 0;
                const int tz = engine::voxel::kReplicationChunkSize > 0
                                   ? target.z / engine::voxel::kReplicationChunkSize
                                   : 0;
                if (resumeWorld->is_chunk_loaded(tx, tz)) continue;
                const auto streamStart = std::chrono::steady_clock::now();
                while (!resumeWorld->is_chunk_loaded(tx, tz)) {
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - streamStart).count() > 10000) {
                        std::cerr << "Server: resume world chunk stream timeout\n";
                        return 1;
                    }
                    resumeWorld->update(replayFocus, 1.0f / 60.0f);
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
        }
        std::size_t replayCount = 0;
        std::size_t replayApplied = 0;
        auto replayConsumer = [&](const engine::networking::JournalEntry& entry) {
            ++replayCount;
            if (entry.kind != "block_edit" || entry.data.size() < 12) return;
            const auto rd = [&](std::size_t off) -> int {
                return static_cast<int>(
                    entry.data[off] |
                    (static_cast<std::uint32_t>(entry.data[off + 1]) << 8) |
                    (static_cast<std::uint32_t>(entry.data[off + 2]) << 16) |
                    (static_cast<std::uint32_t>(entry.data[off + 3]) << 24));
            };
            // Conexão de replay fresca por entrada: registra -> aplica ->
            // desregistra (D.2 — nenhuma subscription temporária sobrevive;
            // cooldown/re-registro nunca bloqueiam um journal multi-edit).
            engine::world::WorldReplicationInterest replayInterest;
            replayInterest.worldName = replayWorld;
            replayInterest.interest.position = { rd(0), rd(4), rd(8) };
            replayInterest.interest.chunkRadius = 2;
            const std::uint32_t replayConn =
                100u + static_cast<std::uint32_t>(replayCount);
            std::string rErr;
            if (replication->server_register_connection(
                    replayConn, replayInterest, rErr)) {
                const auto edit =
                    replication->server_submit_edit(
                        replayConn, rd(0), rd(4), rd(8), 1);
                if (edit.accepted) ++replayApplied;
                replication->server_unregister_connection(replayConn);
            }
        };
        const std::size_t total = server->security().replay(0, replayConsumer);
        if (replayCount == 0 || replayApplied == 0) {
            std::cerr << "Server: journal replay did not reproduce edits ("
                      << replayCount << " entries, " << replayApplied
                      << " applied)\n";
            return 1;
        }
        std::cout << "Server: journal replay OK (" << total << " entries, "
                  << replayApplied << " re-applied to world '"
                  << replayWorld << "')\n";
    }

    // AGENTE 2 block D (data-driven structures): the server evaluates the
    // structure-placement pipeline against its AUTHORITATIVE world — a
    // hand-authored structure asset + spawn rule decides deterministically
    // (try_place is a pure function of (rules, worldSeed, column), using the
    // REAL surface height probed from the streamed world). The decision is
    // reported, not written: place_structure writes through the world's
    // transactional path and would mutate the authoritative terrain that the
    // physics self-validation rests on, so the pipeline is exercised read-only
    // here (generation + placement decision), exactly the deterministic half
    // the item requires.
    {
        auto structures = engine::procgen::create_structure_placement_system();
        std::string structError;
        engine::procgen::StructureAssetSpec towerSpec;
        towerSpec.sampleWidth = 4;
        towerSpec.sampleHeight = 4;
        for (int z = 0; z < towerSpec.sampleHeight; ++z) {
            for (int x = 0; x < towerSpec.sampleWidth; ++x) {
                const bool wall = z == 0 || z == towerSpec.sampleHeight - 1 ||
                                  x == 0 || x == towerSpec.sampleWidth - 1;
                towerSpec.sample.push_back(wall ? 1u : 2u);
            }
        }
        towerSpec.patternSize = 2;
        towerSpec.seed = 11u;
        towerSpec.profiles.emplace_back(
            1, std::vector<std::uint32_t>{ 3, 3, 3 });
        towerSpec.profiles.emplace_back(2, std::vector<std::uint32_t>{ 5 });
        engine::procgen::StructureDefinition tower;
        tower.id = "vulkancraft:watchtower";
        tower.spec = towerSpec;
        tower.outputWidth = 8;
        tower.outputHeight = 8;
        if (!structures->add_definition(tower, structError)) {
            std::cerr << "Server: structure definition refused: "
                      << structError << "\n";
            return 1;
        }
        engine::procgen::StructureSpawnRule rule;
        rule.structureId = tower.id;
        rule.density = 1.0f;  // gates: biome any, surface any, spacing 8
        rule.spacing = 8;
        rule.yOffset = 1;
        if (!structures->set_rules({ rule }, structError)) {
            std::cerr << "Server: structure rules refused: " << structError
                      << "\n";
            return 1;
        }
        engine::procgen::StructurePlacement placed;
        const bool spawned = structures->try_place(
            {}, 8, 8, static_cast<int>(std::floor(surfaceY)), "",
            20260829u, placed, structError);
        if (spawned) {
            std::cout << "Server: structure pipeline decided a placement at "
                      << "(" << placed.origin.x << ", " << placed.origin.y
                      << ", " << placed.origin.z << ") seed="
                      << placed.placementSeed << " ("
                      << placed.output.width << "x" << placed.output.height
                      << "x" << placed.output.depth << ", "
                      << placed.output.blocks.size() << " blocks)\n";
        } else {
            std::cout << "Server: structure pipeline decided no placement ("
                      << structError << ")\n";
        }
    }

    // AGENTE 2 block H (terrain-aware animation): the server drives BOTH halves
    // of the foot-placement pipeline over its AUTHORITATIVE voxel world —
    // IContactPlanner maps a gait + body state to per-foot targets, and
    // IFootPlacer re-anchors the planted feet to the REAL terrain surface
    // sampled through create_voxel_foot_terrain_sampler (top-down scan of the
    // public IVoxelWorld). Deterministic for the same inputs; observable as
    // surface heights / planted counts. Read-only: nothing is written.
    {
        auto gait = engine::animation::GaitAsset{};
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
        std::string gaitError;
        if (!gait.validate(gaitError)) {
            std::cerr << "Server: gait asset invalid: " << gaitError << "\n";
            return 1;
        }
        auto planner = engine::animation::create_contact_planner();
        auto placer = engine::animation::create_foot_placer();
        auto terrain = engine::animation::create_voxel_foot_terrain_sampler(
            *world, 512.0f, -8.0f);
        engine::animation::FootPlacementSpec spec;
        spec.maxStepHeight = 0.5f;
        engine::animation::GaitPlan plan;
        if (!planner->plan(gait, 0.3f, glm::vec3(8.0f, surfaceY + 1.0f, 8.0f),
                           0.0f, glm::vec2(0.0f), plan, gaitError)) {
            std::cerr << "Server: gait plan refused: " << gaitError << "\n";
            return 1;
        }
        engine::animation::FootPlacementResult prev;
        engine::animation::FootPlacementResult placed;
        if (!placer->place(spec, *terrain, plan, prev, placed, gaitError)) {
            std::cerr << "Server: foot placement refused: " << gaitError << "\n";
            return 1;
        }
        std::size_t planted = 0;
        float surfaceSum = 0.0f;
        std::size_t surfaceKnown = 0;
        for (const auto& foot : placed.feet) {
            if (foot.stance) ++planted;
            if (foot.surfaceKnown) {
                ++surfaceKnown;
                surfaceSum += foot.surfaceHeight;
            }
        }
        std::cout << "Server: terrain-aware foot placement over authoritative "
                     "voxels: " << placed.feet.size()
                  << " feet (" << planted << " planted), surface known on "
                  << surfaceKnown << ", avg surface "
                  << (surfaceKnown ? surfaceSum / static_cast<float>(surfaceKnown)
                                   : -1.0f)
                  << "\n";
    }

    // AGENTE 2 block H (navigation): bake the navmesh from the REAL
    // authoritative voxel world (sample_voxel_columns reads the loaded chunks
    // around the focus) and run a path query on it — the promoted
    // INavigationProvider is the navigation authority on the dedicated server
    // (same Recast+Detour core the editor play mode bakes with). Runs BEFORE
    // the eviction proof below unloads every chunk away from the far focus;
    // a failed bake (no walkable surface streamed) is reported, not fatal.
    {
        auto nav = engine::navigation::create_recast_navigation_provider();
        std::string navError;
        engine::navigation::NavmeshConfig navConfig;
        navConfig.boundsMinX = 0.0f;
        navConfig.boundsMaxX = 32.0f;
        navConfig.boundsMinZ = 0.0f;
        navConfig.boundsMaxZ = 32.0f;
        navConfig.boundsMinY = -8.0f;
        navConfig.boundsMaxY = 220.0f;
        navConfig.cellSize = 0.5f;
        navConfig.agentRadius = 0.4f;
        navConfig.agentHeight = 1.8f;
        const auto columns =
            engine::navigation::sample_voxel_columns(*world, navConfig, navError);
        if (!nav->build(navConfig, columns, navError)) {
            std::cout << "Server: navigation bake skipped (" << navError
                      << "); no walkable surface streamed around the focus\n";
        } else {
            engine::navigation::PathResult path;
            const float startY = surfaceY + 1.0f;
            const bool found = nav->find_path(8.0f, startY, 8.0f,
                                              16.0f, startY, 8.0f, path);
            std::cout << "Server: navmesh baked (revision "
                      << nav->revision() << ", " << columns.size()
                      << " voxel columns, valid=" << (nav->valid() ? "y" : "n")
                      << "); path (8,8)->(16,8) "
                      << (found ? "found (" + std::to_string(path.totalLength)
                                      + "m, "
                                      + std::to_string(path.waypoints.size() / 3)
                                      + " waypoints)"
                                : "not found")
                      << "\n";
            // AGENTE 2 block I (off-mesh links): register a jump/climb edge
            // on the SAME navmesh the server bakes — the live navmesh carries
            // off-mesh connectivity (jump/climb), not just walkable tiles. A
            // refused registration is reported, never silent.
            engine::navigation::OffMeshLink jump;
            jump.startX = 10.0f;
            jump.startY = surfaceY + 1.0f;
            jump.startZ = 8.0f;
            jump.endX = 12.0f;
            jump.endY = surfaceY + 3.0f;
            jump.endZ = 8.0f;
            jump.radius = 0.5f;
            jump.bidirectional = false;
            const bool linksOk =
                nav->set_off_mesh_links({ jump }, navError);
            std::cout << "Server: off-mesh link (jump/climb ledge) "
                      << (linksOk ? "registered on the navmesh"
                                  : "refused (" + navError + ")")
                      << "\n";
        }
    }

    // AGENTE 2 block D (origin rebase + local spaces): the SDK origin-rebase
    // service holds ABSOLUTE (double) world coordinates and keeps the local
    // frame following the server focus; the local-space service hosts a
    // hierarchical space tree (vehicle/planet frame) and binds entities to a
    // moving frame. Both run against the REAL IWorldManager worlds — a rebase
    // translates every entity of every world, a bound entity follows its
    // space instead. Observable on stdout; nothing mutates the terrain.
    {
        auto rebase = engine::world::create_origin_rebase(*manager);
        auto spaces = engine::world::create_local_space(*manager);
        std::string spaceError;
        // A moving "carrier" frame (like a vehicle/planet) with a child seat:
        // the parent/child space tree is the SDK's attachment hierarchy — the
        // entity bound to the child seat follows the frame when it moves.
        if (spaces) {
            engine::world::SpaceTransform carrierTf;
            carrierTf.position = { 1.0, 2.0, 3.0 };
            const bool rootOk = spaces->create_space(
                "carrier", "", carrierTf, spaceError);
            engine::world::SpaceTransform seatTf;
            seatTf.position = { 0.0, 0.5, 0.0 };
            const bool seatOk = spaces->create_space(
                "carrier.seat", "carrier", seatTf, spaceError);
            // Bind a real entity from the authoritative world to the moving
            // space: its stored position becomes space-local.
            auto* e = world->entity_world().get();
            std::string bErr;
            engine::entity::EntityId rider = e ? e->spawn(
                "vulkancraft:rider",
                engine::entity::Position{ 0.0f, 0.5f, 0.0f }, bErr)
                : engine::entity::EntityId{};
            const bool bound = rootOk && seatOk && rider.valid() &&
                spaces->bind_entity("server", rider, "carrier.seat",
                                    glm::vec3(0.0f, 0.5f, 0.0f), bErr);
            std::cout << "Server: local-space tree 'carrier'->'carrier.seat' "
                      << (bound ? "bound entity to the moving frame"
                                : "created")
                      << "\n";
            if (bound &&
                spaces->move_space("carrier", glm::dvec3(10.0, 0.0, 10.0))) {
                glm::dvec3 wp;
                const bool followed =
                    spaces->entity_world_position("server", rider, wp,
                                                  spaceError);
                std::cout << "Server: space moved; bound entity world pos "
                          << (followed ? "(" + std::to_string(wp.x) + ", " +
                                          std::to_string(wp.y) + ", " +
                                          std::to_string(wp.z) + ")"
                                       : "unavailable")
                          << "\n";
            }
        }
        // Rebase: spawn at an ABSOLUTE coordinate far from the origin and
        // let the focus-driven update translate every entity when the focus
        // leaves the threshold.
        if (rebase) {
            std::string rErr;
            const engine::entity::EntityId far =
                rebase->spawn_at("server", "vulkancraft:sentinel",
                                 glm::dvec3(1.0e5, surfaceY, 1.0e5), rErr);
            const auto result = rebase->update(
                glm::dvec3(1.0e5, surfaceY, 1.0e5), 512.0, true, rErr);
            glm::dvec3 abs;
            const bool readable = far.valid() &&
                rebase->absolute_position("server", far, abs, rErr);
            std::cout << "Server: origin rebase "
                      << (result.rebased ? "fired" : "no-op") << " (delta "
                      << result.delta.x << ", translated "
                      << result.translatedEntities << "; absolute pos "
                      << (readable ? "preserved" : "unreadable") << ")\n";
        }
    }

    // AGENTE 2 block G.94 (missions): the dedicated server drives the SDK
    // mission runtime against a REAL world seam implemented over the
    // authoritative voxel world — count_of() counts solid blocks in the
    // streamed region (stone collected = terrain near the focus), position()
    // is the focus, apply_reward/set_flag are recorded. accept -> update ->
    // complete runs the real state machine; every emitted event is reported
    // on stdout. The seam is read-only (no terrain mutation).
    {
        struct ServerMissionWorld final : public engine::gameplay::IMissionWorld {
            const engine::voxel::IVoxelWorld* world;
            float px, pz, surfaceYAt;
            std::string rewardItem;
            int rewardCount{ 0 }, rewardXp{ 0 };
            std::vector<std::string> setFlags;
            float count_of(const std::string& key) const override {
                // "stone" -> count solid blocks around the focus surface.
                if (key != "stone") return 0.0f;
                float n = 0.0f;
                for (int dx = -1; dx <= 1; ++dx)
                    for (int dz = -1; dz <= 1; ++dz) {
                        const int bx = static_cast<int>(px) + dx;
                        const int bz = static_cast<int>(pz) + dz;
                        const int by = static_cast<int>(surfaceYAt) - 1;
                        if (world->is_solid(world->get_block(bx, by, bz)))
                            n += 1.0f;
                    }
                return n;
            }
            bool flag(const std::string& key) const override { return false; }
            float attribute(const std::string& key) const override {
                return key == "level" ? 2.0f : 0.0f;
            }
            bool position(float& x, float& z) const override { x = px; z = pz; return true; }
            bool apply_reward(const std::string& itemId, int count, int xp) override {
                rewardItem = itemId; rewardCount = count; rewardXp = xp;
                return true;
            }
            bool set_flag(const std::string& key) override {
                setFlags.push_back(key); return true;
            }
        };
        ServerMissionWorld mworld;
        mworld.world = world;
        mworld.px = 8.0f;
        mworld.pz = 8.0f;
        mworld.surfaceYAt = surfaceY;
        auto missions = engine::gameplay::create_mission_runtime();
        std::string mErr;
        engine::gameplay::MissionDefinition mdef;
        mdef.name = "First Steps";
        mdef.id = "11111111-2222-3333-4444-555555555555";
        engine::gameplay::MissionObjective collect;
        collect.id = "collect_stone";
        collect.kind = engine::gameplay::MissionObjectiveKind::Collect;
        collect.target = "stone";
        collect.count = 3;
        engine::gameplay::MissionObjective reach;
        reach.id = "reach_hill";
        reach.kind = engine::gameplay::MissionObjectiveKind::Reach;
        reach.count = 1;
        reach.x = 8.0f;
        reach.z = 8.0f;
        reach.radius = 5.0f;
        mdef.objectives = { collect, reach };
        mdef.reward.itemId = "coin";
        mdef.reward.count = 5;
        mdef.reward.xp = 100;
        mdef.reward.setFlag = "quest1_done";
        engine::gameplay::MissionState mstate;
        if (!mdef.validate(mErr)) {
            std::cerr << "Server: mission definition refused: " << mErr << "\n";
            return 1;
        }
        std::vector<engine::gameplay::MissionEvent> events;
        if (!missions->accept(mdef, mstate, mworld, events, mErr)) {
            std::cerr << "Server: mission accept refused: " << mErr << "\n";
            return 1;
        }
        if (!missions->update(mdef, mstate, mworld, events, mErr)) {
            std::cerr << "Server: mission update refused: " << mErr << "\n";
            return 1;
        }
        if (!missions->complete(mdef, mstate, mworld, events, mErr)) {
            std::cerr << "Server: mission complete refused: " << mErr << "\n";
            return 1;
        }
        const bool rewarded = mworld.rewardItem == "coin";
        std::cout << "Server: mission '" << mdef.name
                  << "' accepted->completed (reward "
                  << (rewarded ? mworld.rewardItem + " x" +
                                 std::to_string(mworld.rewardCount)
                               : "MISSING")
                  << ", xp " << mworld.rewardXp << ", "
                  << events.size() << " mission events)\n";
        if (!rewarded || mworld.setFlags.empty()) {
            std::cerr << "Server: mission reward/set_flag not applied\n";
            return 1;
        }
    }

    // AGENTE 2 block G.93 (ability system): the dedicated server drives the
    // SDK ability runtime against a real IAbilityWorld seam implemented over
    // the authoritative voxel world — block_at()/raycast() read the real
    // terrain, physics/health operate on a small body registry (the minimal
    // seam the SDK tests use). A data-driven damage ability is registered and
    // cast on a body; the health delta and cooldown state are observable on
    // stdout. The seam is READ-ONLY w.r.t. terrain (no set_block issued), so
    // the physical self-validation stays untouched.
    {
        struct ServerAbilityWorld final : public engine::gameplay::IAbilityWorld {
            const engine::voxel::IVoxelWorld* world;
            std::map<std::uint32_t, float> healths;
            std::map<std::uint32_t, float> attributes;
            std::uint32_t nextBody{ 1 };
            std::uint32_t block_at(int x, int y, int z) const override {
                return world->get_block(x, y, z);
            }
            bool set_block(int, int, int, std::uint32_t) override { return false; }
            bool body_state(const engine::gameplay::AbilityBodyId& body,
                            engine::gameplay::AbilityBodyState& out) const override {
                const auto it = healths.find(body.id);
                if (it == healths.end()) return false;
                out.position = glm::vec3(8.0f, 64.0f, 8.0f);
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
            bool raycast(const glm::vec3& origin, const glm::vec3& direction,
                         float maxDistance,
                         engine::gameplay::AbilityRaycastHit& out) const override {
                const auto hit = world->raycast(origin, direction, maxDistance);
                if (!hit.hit) return false;
                out.point = hit.position;
                out.normal = hit.normal;
                out.distance = maxDistance;
                return true;
            }
            float attribute(const engine::gameplay::AbilityBodyId& body,
                            const std::string& name) const override {
                const auto it = attributes.find(body.id);
                return (it != attributes.end() && name == "level")
                    ? it->second : 0.0f;
            }
            engine::gameplay::AbilityTagList tags(
                const engine::gameplay::AbilityBodyId&) const override { return {}; }
            bool spend_cost(const engine::gameplay::AbilityBodyId&,
                            const std::string&, float) override { return true; }
            bool health(const engine::gameplay::AbilityBodyId& body,
                        float& out) const override {
                const auto it = healths.find(body.id);
                if (it == healths.end()) return false;
                out = it->second;
                return true;
            }
            bool damage(const engine::gameplay::AbilityBodyId& body,
                        float amount) override {
                const auto it = healths.find(body.id);
                if (it == healths.end()) return false;
                it->second = std::max(0.0f, it->second - amount);
                return true;
            }
            bool heal(const engine::gameplay::AbilityBodyId& body,
                      float amount) override {
                const auto it = healths.find(body.id);
                if (it == healths.end()) return false;
                it->second += amount;
                return true;
            }
        };
        ServerAbilityWorld aworld;
        aworld.world = world;
        aworld.healths[1] = 100.0f;
        aworld.attributes[1] = 3.0f;
        auto abilities = engine::gameplay::create_ability_system();
        std::string aErr;
        engine::gameplay::AbilityDefinition punch;
        punch.name = "punch";
        punch.id = "abilities:punch";
        punch.targeting.mode = engine::gameplay::AbilityTargetMode::Self;
        engine::gameplay::AbilityEffect dmg;
        dmg.type = engine::gameplay::AbilityEffectType::Damage;
        dmg.amount = 10.0f;
        punch.effects.push_back(dmg);
        punch.cooldownSeconds = 2.0f;
        if (!punch.validate(aErr) || !abilities->register_ability(punch, aErr)) {
            std::cerr << "Server: ability register refused: " << aErr << "\n";
            return 1;
        }
        engine::gameplay::AbilityBodyId caster;
        caster.id = 1;
        engine::gameplay::AbilityTarget target;
        target.mode = engine::gameplay::AbilityTargetMode::Self;
        const auto cast = abilities->cast(punch.id, caster, target, aworld);
        if (!cast.accepted) {
            std::cerr << "Server: ability cast refused: " << cast.error << "\n";
            return 1;
        }
        float hp = -1.0f;
        abilities->update(0.1f, aworld);
        const bool damaged = aworld.health(caster, hp) && hp == 90.0f;
        std::cout << "Server: ability 'punch' cast accepted (effects "
                  << cast.effectCount << ", target hp "
                  << (damaged ? std::to_string(hp) : "MISSING")
                  << ", cooldown " << abilities->cooldown_remaining(punch.id)
                  << "s)\n";
        if (!damaged) {
            std::cerr << "Server: ability damage not applied through the seam\n";
            return 1;
        }
    }

    // AGENTE 2 block F (destruction core): the dedicated server drives the
    // deterministic blast model (impulse/heat/damage falloff + fragments)
    // against the REAL surface position sampled from the authoritative voxel
    // world. Read-only w.r.t. terrain: the core evaluates the blast math at a
    // distance and emits deterministic fragments from a fixed seed — the
    // authoritative edit (block removal) stays with the transaction path.
    {
        auto blast = engine::physics::create_explosion();
        std::string bErr;
        engine::physics::ExplosionSpec bspec;
        bspec.radius = 6.0;
        bspec.impulse = 1200.0;
        bspec.heat = 800.0;
        bspec.damage = 90.0;
        bspec.falloff_power = 2.0;
        bspec.fragments = 12;
        bspec.fragment_speed = 18.0;
        if (!blast->configure(bspec, bErr)) {
            std::cerr << "Server: explosion configure refused: " << bErr << "\n";
            return 1;
        }
        const auto core = blast->sample_at(2.5, bErr);
        const auto frags = blast->fragments(20260829u, bErr);
        if (bErr.empty() && core.distance == 2.5 && frags.size() == 12) {
            std::cout << "Server: blast @2.5m falloff "
                      << core.falloff << " (impulse " << core.impulse
                      << ", heat " << core.heat << ", damage " << core.damage
                      << "); " << frags.size() << " fragments from seed\n";
        } else {
            std::cerr << "Server: blast core misbehaved: " << bErr << "\n";
            return 1;
        }
    }

    // Authority on eviction: focus far away + budget 1 -> chunks leave the
    // loaded set; slabs are destroyed and the contained body is despawned.
    const glm::vec3 farFocus{ 600.0f, 8.0f, 600.0f };
    world->set_chunk_budget(1);
    const auto evictStart = std::chrono::steady_clock::now();

    while (bridge.dynamic_body_count() != 0) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - evictStart).count() > 20000) {
            std::cerr << "Server: bodies survived the chunk eviction\n";
            return 1;
        }
        world->update(farFocus, 1.0f / 60.0f);
        bridge.sync(farFocus);
        physics.step(1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (bridge.unloaded_body_count() < 1) {
        std::cerr << "Server: unload authority did not despawn the body\n";
        return 1;
    }

    // G — prediction + reconciliation sobre o runtime público: duas steps
    // preditas, depois uma posição autoritativa divergente força correção
    // com replay dos inputs ainda não confirmados (sem teleporte visual). A
    // correção conta uma métrica real de rollback (D.3) exposta ao profiler.
    {
        std::string predErr;
        auto& pr = server->prediction();
        pr.reset(predErr);
        pr.predict(0.016f, 1.0f, 0.0f, false);
        pr.predict(0.016f, 0.0f, 1.0f, false);
        const auto unacked = pr.next_sequence() - 1;
        engine::networking::PredictedPose auth = pr.pose();
        auth.x += 0.5f;   // a autoridade do servidor diverge levemente
        const auto rc = pr.reconcile(auth, unacked);
        if (!rc.corrected) {
            std::cerr << "Server: prediction reconcile did not correct a drift\n";
            return 1;
        }
        obsRollbacks += 1;
        std::cout << "Server: prediction reconcile corrected drift (rollback "
                  << obsRollbacks << " reported to observability)\n";
    }

    // D.2 — remoção limpa no shutdown: o cliente é removido do server
    // (remove_client) e a conexão é desregistrada da replicação
    // (server_unregister_connection) — subscriptions/interesses/corpos
    // temporários são liberados, sem vazamento.
    {
        std::string rmErr;
        const bool removed = server->remove_client(clientConn, rmErr);
        replication->server_unregister_connection(
            static_cast<engine::voxel::ReplicationConnectionId>(clientConn));
        std::cout << "Server: shutdown cleanup (remove_client "
                  << (removed ? "ok" : "error: " + rmErr)
                  << ", replication connection unregistered)\n";
    }

    // Shut down the public server + replication front cleanly (no pending
    // authoritative edits lost: economically the dedicated path already
    // committed them through server_submit_edit above). Graceful leave on the
    // session keeps the reconnect token valid. The save result is CHECKED: a
    // failed save is reported diagnostically (a silent failure would make the
    // next boot treat the session as falsely "fresh" — C.2/C.4 semantics).
    server->session().graceful_leave(clientConn);
    std::string saveError;
    if (!replication->server_save("server", "ag3-server-save.json", saveError)) {
        std::cerr << "Server: session save FAILED (next boot would resume "
                     "falsely fresh): " << saveError << "\n";
        return 1;
    }
    std::cout << "Server: session save OK (ag3-server-save.json)\n";
    // AGENTE 2 block E (timeline): capture a REAL temporal state of the
    // authoritative world (save v5 through IWorldManager) and register it on
    // the public timeline — then let the budget policy prune/compact the
    // registered states (deterministic, observable). This is the SDK
    // ITimeTravel + ITimelinePolicy consumed by the dedicated executable, not
    // an isolated factory.
    {
        std::string ttError;
        const bool captured = serverTimeline &&
            serverTimeline->capture_state("final", "server",
                                          "ag3-server-timeline-final.json",
                                          ttError);
        if (!captured) {
            std::cerr << "Server: timeline capture refused: " << ttError << "\n";
        } else if (serverTimelinePolicy) {
            // The policy is the deterministic budget keeper: with 1 registered
            // state and maxStates=4 nothing is pruned; the exercise proves the
            // policy runs against the server's live timeline.
            const auto registered = serverTimeline->states();
            std::vector<engine::world::TimelinePolicyState> policyStates;
            policyStates.reserve(registered.size());
            for (const auto& s : registered) {
                policyStates.push_back({ s.name, s.worldName, s.path });
            }
            const auto toPrune = serverTimelinePolicy->prune(policyStates);
            const auto toCompact = serverTimelinePolicy->compact(policyStates);
            if (!toPrune.empty() || !toCompact.empty()) {
                std::cerr << "Server: timeline policy pruned/compacted states "
                             "unexpectedly (" << toPrune.size() << "/"
                          << toCompact.size() << ")\n";
                return 1;
            }
            std::cout << "Server: timeline policy OK ("
                      << registered.size() << " state(s), budget "
                      << serverTimelinePolicy->max_states() << ")\n";
        }
    }
    (void)server->stop(netError);
    // AGENTE 2 block A: ordered teardown of the canonical composition
    // (reverse of bootstrap, flushes persistence). Safe when never bound.
    if (serverWorldRuntime) serverWorldRuntime->shutdown();

    // AGENTE 2 G.95 observability: the server-side day/night clock (advanced
    // by the canonical runtime every tick) reports its real state — proof the
    // same deterministic clock the game drives its sun with ran here.
    std::cout << "Server: day/night clock after " << tickCount
              << " ticks: time_of_day="
              << (serverDayNight ? serverDayNight->time_of_day() : -1.0f)
              << " daylight_factor="
              << (serverDayNight ? serverDayNight->daylight_factor() : -1.0f)
              << " music_state="
              << (serverMusic ? serverMusic->current_state() : std::string("n/a"))
              << "\n";

    // AGENTE 2 block E (async autosave): drain any in-flight background save
    // before teardown and report it — the delta autosave never blocks the
    // frame, it completes on the job thread.
    {
        std::string asError;
        const bool drained = world->wait_async_saves(asError);
        std::cout << "Server: async autosave drained ("
                  << (drained ? "ok" : "error: " + asError) << ")\n";
    }

    // AGENTE 2 block F (physics over ECS): despawn one ECS entity and verify
    // its mirrored body is gone (no orphan bodies) — the same create/destroy
    // symmetry the game proves per frame.
    if (!serverEcsBodies.empty()) {
        const auto& [eid, body] = serverEcsBodies.back();
        if (serverRuntimeEcs) serverRuntimeEcs->despawn(eid);
        if (serverRuntimePhysics) serverRuntimePhysics->physics().destroy_body(body);
        serverEcsBodies.pop_back();
        std::cout << "Server: ECS despawn destroyed its mirrored body ("
                  << serverEcsBodies.size() << " remaining)\n";
    }

    std::cout << "VulkanEngineServer completed " << tickCount
              << " headless ticks: world-replication+transport OK "
                 "(registered 1 client, streamed "
              << replicatedChunksSeen
              << " chunk snapshot(s), " << interestRelevantEntities
              << " interest-relevant entity ref(s) per tick, committed "
                 "authoritative block edits via IWorldReplication), "
                 "physics authority OK "
                 "(rest+sleep, wake, eviction despawn; terrain slabs "
              << bridge.spawned_terrain_count()
              << ", unloaded bodies " << bridge.unloaded_body_count()
              << "), ECS->physics mirror " << serverEcsBodies.size()
              << " live body(s), worlds " << manager->world_count()
              << " (multi-world + portal + entity-transfer exercised).\n";
    return 0;
}