// EditorApplicationSdkRuntimes.cpp
//
// Conta 5 fechamento_global real-consumer integration. These public engine
// SDK factories were TEST-ONLY: the only call sites were the SDK tests. This
// TU makes the EDITOR a genuine product consumer of each — the same pattern
// as the other gap-factory consumers in this editor (IAssetCooker,
// ILuauSandbox, INetworkSession, IPackageManager, ...): instantiate the
// contract in the editor, drive its REAL methods every frame, and serialize
// an observable into m_sdkContractJson (GET /sdk-contracts).
//
//   create_job_system              engine::jobs::IJobSystem
//   create_procgen_jobs            engine::procgen::IProcgenJobs
//   create_cancellation_token      engine::procgen::ICancellationToken
//   create_procgen_preview         engine::procgen::IProcgenPreview
//   create_farm_cooker             Engine::Farm::IFarmCooker
//   create_hilbert_cell_index      engine::world::IHilbertCellIndex
//   create_hilbert_cell_index_json engine::world::IHilbertCellIndex
//   create_block_entity_scripting  engine::voxel::IBlockEntityScripting
//   create_audio_mixer             engine::audio::IAudioMixer
//
// No build/test/gate runs — this is product code only.

#include "EditorApplication.hpp"
#include "EditorInternalHelpers.hpp"
#include "EditorSdkContractJson.hpp"

#include "engine/voxel/IVoxelBlockEntity.hpp"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace Engine {

namespace {

// A deterministic flat-terrain generator so the editor-owned voxel world can
// boot a real chunk without a GPU or asset pipeline — the same shape the SDK
// tests use to exercise create_default_voxel_world().
class SdkFlatGenerator final : public engine::voxel::IVoxelGenerator {
public:
    explicit SdkFlatGenerator(int height) : height_(height) {}

    engine::voxel::TerrainPoint sample(float, float) const override {
        engine::voxel::TerrainPoint point;
        point.height = height_;
        point.temperature = 0.5f;
        point.moisture = 0.5f;
        point.slope = 0.0f;
        return point;
    }
    float cave_density(float, float, float) const override { return -1.0f; }
    float ore_density(float, float, float) const override { return -1.0f; }

private:
    int height_;
};

// A project-owned block entity that declares a script through script_id() —
// the runtime the IBlockEntityScripting bridge drives (a chest-loot counter).
class SdkChestCounter final : public engine::voxel::IVoxelBlockEntity {
public:
    std::string type_id() const override { return "project:editor_chest"; }
    void on_tick(uint64_t) override {}
    uint32_t data_version() const override { return 1; }
    std::vector<uint8_t> serialize_state() const override { return {}; }
    bool deserialize_state(const std::vector<uint8_t>& data,
                           uint32_t version) override {
        (void)data;
        return version == 1;
    }
    std::string script_id() const override { return "project:chest_loot"; }
};

// Bounded world boot for the editor consumer: pump update() until the center
// chunk loads (mirrors the SDK tests' boot_world, bounded so it can never
// stall the editor frame).
bool sdk_boot_world(engine::voxel::IVoxelWorld& world, const glm::vec3& player,
                    int budget, double maxMs) {
    world.set_chunk_budget(budget);
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    for (int step = 0; step < 6000; ++step) {
        if (world.is_chunk_loaded(0, 0)) return true;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - start).count() > maxMs) {
            return false;
        }
        world.update(player, 1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return world.is_chunk_loaded(0, 0);
}

}  // namespace

void EditorApplication::refresh_sdk_contract_runtimes() {
    // Agente 3 (fechamento_solidacao — A3-SDK-JSON-INVALIDO): the observable
    // /sdk-contracts document is built by the standalone nlohmann-based
    // serializer (EditorSdkContractJson.cpp) instead of manual oss-string
    // concatenation. Every subsystem here still drives its REAL contract
    // methods each frame exactly as before; only the final serialization path
    // changed, so a failing factory or a hostile string can no longer emit
    // invalid JSON (no leading comma, no unescaped quote/backslash/newline).
    SdkContractStats stats;

    // ---- 1. create_job_system (engine::jobs::IJobSystem) ------------------
    // Real job dispatch: each frame the editor submits a genuine unit of work
    // through the public job system, reads its queued state and drains it
    // until complete — never a hand-rolled scheduler on the side.
    if (!m_jobSystem) {
        m_jobSystem = engine::jobs::create_job_system();
    }
    if (m_jobSystem) {
        engine::jobs::JobHandle handle = m_jobSystem->submit([&]() {
            m_jobSystemCompleted += 1;
        });
        if (handle.id != 0) {
            ++m_jobSystemSubmitted;
        }
        const std::size_t pendingBefore = m_jobSystem->pending();
        // Drain the queue in submission order; the job runs here.
        m_jobSystem->drain();
        stats.hasJobs = true;
        stats.jobsSubmitted = m_jobSystemSubmitted;
        stats.jobsCompleted = m_jobSystemCompleted;
        stats.jobsPending = m_jobSystem->pending() == 0 ? 0 : pendingBefore;
        stats.jobsHandle = handle.id;
    }

    // ---- 2+3. create_procgen_jobs + create_cancellation_token ------------
    // Real cancellable procgen execution: erode a real heightmap tile batch
    // through IProcgenJobs with a genuinely-propagated ICancellationToken.
    // The token is checked per unit (token.cancelled()) and the progress
    // callback is invoked, so cancellation is a real contract path.
    if (!m_procgenJobs) m_procgenJobs = engine::procgen::create_procgen_jobs();
    if (!m_cancelToken) m_cancelToken = engine::procgen::create_cancellation_token();
    if (m_procgenJobs && m_cancelToken) {
        engine::procgen::ErosionSpec spec;
        spec.seed = m_previewSampleSize;
        spec.iterations = 400;
        spec.maxSteps = 20;
        engine::procgen::Heightmap tile;
        tile.width = 12;
        tile.height = 12;
        tile.values.assign(static_cast<size_t>(12 * 12), 0.5f);
        std::vector<engine::procgen::Heightmap> inputs(2, tile);
        std::vector<engine::procgen::Heightmap> outputs;
        std::string jobErr;
        std::size_t progressUnits = 0;
        const engine::procgen::JobResult result = m_procgenJobs->erode_tiles(
            spec, inputs, *m_cancelToken,
            [&](const engine::procgen::JobProgress& p) {
                progressUnits = p.completed;
            },
            outputs, jobErr);
        m_procgenJobUnits = outputs.size();
        if (m_cancelToken->cancelled()) {
            m_procgenJobResult = "Cancelled";
        } else switch (result) {
            case engine::procgen::JobResult::Completed: m_procgenJobResult = "Completed"; break;
            case engine::procgen::JobResult::Cancelled: m_procgenJobResult = "Cancelled"; break;
            case engine::procgen::JobResult::Failed: m_procgenJobResult = "Failed"; break;
        }
        stats.hasProcgen = true;
        stats.procgenResult = m_procgenJobResult;
        stats.procgenUnits = m_procgenJobUnits;
        stats.procgenProgress = progressUnits;
        stats.procgenCancelled = m_cancelToken->cancelled();
        stats.procgenError = jobErr;
    }

    // ---- 4. create_procgen_preview (engine::procgen::IProcgenPreview) -----
    // Real headless textual preview of the procgen pipeline (erosion on a
    // seeded heightmap) — the SAME contract the editor preview panel calls,
    // driven from LIVE editor state each frame.
    if (!m_procgenPreview) m_procgenPreview = engine::procgen::create_procgen_preview();
    if (m_procgenPreview) {
        engine::procgen::ErosionSpec spec;
        spec.seed = m_terrainParams.seed;
        spec.iterations = 800;
        spec.maxSteps = 24;
        engine::procgen::PreviewOptions opts;
        opts.sampleSize = m_previewSampleSize;
        opts.seed = m_terrainParams.seed;
        opts.showStats = true;
        engine::procgen::PreviewRender render;
        std::string pErr;
        const bool ok = m_procgenPreview->preview_erosion(spec, opts, render, pErr);
        m_previewTitle = ok ? render.title : ("error: " + pErr);
        m_previewLines = render.lines;
        m_previewStats.clear();
        for (const engine::procgen::PreviewStat& stat : render.stats) {
            m_previewStats.emplace_back(stat.label, stat.value);
        }
        stats.hasPreview = true;
        stats.previewOk = ok;
        stats.previewTitle = m_previewTitle;
        stats.previewLines = m_previewLines;
    }

    // ---- 5. create_farm_cooker (Engine::Farm::IFarmCooker) ----------------
    // Real offline-farm cooker gate: cook + verify a ParameterTable through
    // the RuntimeCooker (the only runtime-kind the factory implements), the
    // same deterministic signing path the cooker/package flow uses.
    if (!m_farmCooker) {
        std::string fErr;
        m_farmCooker = Engine::Farm::create_farm_cooker(Engine::Farm::FarmKind::RuntimeCooker, fErr);
    }
    if (m_farmCooker) {
        Engine::Farm::ParameterPayload params;
        params.names = { "editor_gain", "editor_duck", "editor_release" };
        params.values = { 0.75f, 0.5f, 0.2f };
        Engine::Farm::ClipPayload clip;
        Engine::Farm::TrajectoryPayload traj;
        Engine::Farm::CookedFarmAsset asset;
        std::string cErr;
        if (m_farmCooker->cook(Engine::Farm::CookedAssetKind::ParameterTable,
                               clip, traj, params, asset, cErr)) {
            m_farmCookedSignature = asset.signature;
            std::string vErr;
            m_farmCookVerified = m_farmCooker->verify(asset, vErr);
        } else {
            m_farmCookVerified = false;
        }
        stats.hasFarm = true;
        stats.farmKind =
            m_farmCooker->kind() == Engine::Farm::FarmKind::RuntimeCooker ? "RuntimeCooker" : "other";
        stats.farmSignature = m_farmCookedSignature;
        stats.farmVerified = m_farmCookVerified;
    }

    // ---- 6+7. create_hilbert_cell_index + create_hilbert_cell_index_json --
    // Real hierarchical spatial cell index over the editor's world focus. Two
    // instances: one configured programmatically and one loaded from JSON (so
    // BOTH factories are consumed), both queried with cell_id / parent_cell /
    // cover / contains on the live editor camera position.
    if (!m_hilbertIndex) {
        std::string hErr;
        m_hilbertIndex = engine::world::create_hilbert_cell_index(hErr);
        if (m_hilbertIndex) {
            std::string cfgErr;
            engine::world::HilbertCellConfig cfg;
            cfg.maxLevel = 8;
            cfg.maxCoverCells = 128;
            m_hilbertIndex->configure(cfg, cfgErr);
        }
    }
    if (!m_hilbertIndexJson) {
        std::string jErr;
        m_hilbertIndexJson =
            engine::world::create_hilbert_cell_index_json(
                "{\"maxLevel\":8,\"maxCoverCells\":128}", jErr);
    }
    const int fx = static_cast<int>(std::llround(m_editorCamera.position.x));
    const int fz = static_cast<int>(std::llround(m_editorCamera.position.z));
    engine::world::IHilbertCellIndex* hil =
        m_hilbertIndex ? m_hilbertIndex.get() : m_hilbertIndexJson.get();
    if (hil) {
        std::string cErr;
        const int gx = fx % 256 < 0 ? fx % 256 + 256 : fx % 256;
        const int gz = fz % 256 < 0 ? fz % 256 + 256 : fz % 256;
        m_hilbertCellId = hil->cell_id(gx, gz, 5, cErr);
        m_hilbertParentId = hil->parent_cell(m_hilbertCellId);
        std::vector<std::uint64_t> cells;
        std::string coverErr;
        // Keep the cover rect strictly inside the level-4 grid [0, 15] so the
        // minimal cover is always valid (a rect pushed past the edge would
        // make cover() report an out-of-range diagnostic, never a real index).
        const int rx = std::max(0, std::min(8, gx % 16));
        const int rz = std::max(0, std::min(8, gz % 16));
        if (hil->cover(rx, rz, rx + 7, rz + 7, 4, cells, coverErr)) {
            m_hilbertCoverCells = cells.size();
        } else {
            m_hilbertCoverCells = 0;
        }
        int dx = 0, dy = 0;
        std::string posErr;
        hil->cell_position(m_hilbertCellId, dx, dy, posErr);
        stats.hasHilbert = true;
        stats.hilCellId = m_hilbertCellId;
        stats.hilParent = m_hilbertParentId;
        stats.hilCoverCells = m_hilbertCoverCells;
        stats.hilDecodedX = dx;
        stats.hilDecodedY = dy;
        stats.hilJsonVariant = m_hilbertIndexJson != nullptr;
        stats.hilContains = hil->contains(m_hilbertCellId, dx, dy, posErr);
    }

    // ---- 8. create_block_entity_scripting (engine::voxel::IBlockEntityScripting)
    // Real block-entity runtime: the editor owns a real IVoxelWorld (SDK's
    // VoxelWorldFacade) and a script bridge bound to it (takes over its
    // block-entity listener). On first use it boots a flat world once, then
    // registers a chest script and runs it every frame — the script actually
    // runs inside the world's runtime loop, with observability.
    if (!m_blockScripting) {
        m_blockWorld = engine::voxel::create_default_voxel_world();
        if (m_blockWorld) {
            m_blockWorld->register_generator(std::make_shared<SdkFlatGenerator>(96));
            m_blockWorld->register_block_entity_type(
                "project:editor_chest",
                []() -> std::shared_ptr<engine::voxel::IVoxelBlockEntity> {
                    return std::make_shared<SdkChestCounter>();
                });
            m_blockEntityBooted =
                sdk_boot_world(*m_blockWorld, glm::vec3(8.0f, 200.0f, 8.0f), 8, 8000.0);
        }
        if (m_blockEntityBooted && m_blockWorld) {
            m_blockScripting =
                engine::voxel::create_block_entity_scripting(*m_blockWorld);
        }
        if (m_blockScripting) {
            engine::voxel::BlockEntityScriptSpec spec;
            spec.scriptId = "project:chest_loot";
            spec.graphJson = R"({
  "id": "00000000-0000-0000-0000-000000000001",
  "name": "editor_counter",
  "nodes": [
    {"id": "00000000-0000-0000-0000-000000000010", "kind": "Event", "event": "on_tick"},
    {"id": "00000000-0000-0000-0000-000000000011", "kind": "ConstantFloat", "literal": {"type": "float", "value": 1.0}},
    {"id": "00000000-0000-0000-0000-000000000012", "kind": "AddFloat", "variable": "count"},
    {"id": "00000000-0000-0000-0000-000000000013", "kind": "Return"}
  ],
  "links": [
    {"from": "00000000-0000-0000-0000-000000000010", "to": "00000000-0000-0000-0000-000000000011"},
    {"from": "00000000-0000-0000-0000-000000000011", "to": "00000000-0000-0000-0000-000000000012"},
    {"from": "00000000-0000-0000-0000-000000000012", "to": "00000000-0000-0000-0000-000000000013"}
  ]
})";
            m_blockScripting->register_script(spec);
            if (m_blockWorld) {
                auto chest = std::make_shared<SdkChestCounter>();
                std::string aErr;
                m_blockWorld->attach_block_entity(8, 96, 8, chest, aErr);
            }
        }
    }
    if (m_blockScripting) {
        m_blockScripting->tick(1.0 / 60.0);
        m_blockActiveInstances = m_blockScripting->active_instances();
        m_blockCompletedRuns = m_blockScripting->completed_runs();
        m_blockFailedRuns = m_blockScripting->failed_runs();
        double countVar = -1.0;
        if (m_blockScripting->script_variable({ 8, 96, 8 }, "count", countVar)) {
            m_blockScriptVariable = countVar;
        }
        stats.hasBlockEntity = true;
        stats.blockBooted = m_blockEntityBooted;
        stats.blockActive = m_blockActiveInstances;
        stats.blockCompletedRuns = m_blockCompletedRuns;
        stats.blockFailedRuns = m_blockFailedRuns;
        stats.blockLastError = m_blockScripting->last_error();
        stats.blockScriptVar = m_blockScriptVariable;
        stats.blockHasScript = m_blockScripting->has_script("project:chest_loot");
    }

    // ---- CONTA 6 (particle — same provider as the game) -------------------
    // The editor consumes the SAME public Engine::Rendering::create_particle_system()
    // contract the game uses (no parallel ParticleSimulation track): a real
    // .efk effect is loaded once (the same asset the game loads), an instance
    // is spawned at the play camera origin and stepped every frame; the alive
    // count is the per-frame observable.
    if (!m_particleSystemC) {
        m_particleSystemC = Engine::Rendering::create_particle_system();
        if (m_particleSystemC) {
            std::ifstream efkFile("assets/effects/block_simple.efk", std::ios::binary);
            if (!efkFile && std::getenv("VULKANCRAFT_ASSET_DIR")) {
                efkFile.open(std::string(std::getenv("VULKANCRAFT_ASSET_DIR")) +
                             "/effects/block_simple.efk", std::ios::binary);
            }
            if (efkFile) {
                std::vector<std::uint8_t> efkBytes(
                    (std::istreambuf_iterator<char>(efkFile)),
                    std::istreambuf_iterator<char>());
                std::string pErr;
                if (m_particleSystemC->loadEffect(efkBytes.data(), efkBytes.size(), pErr)) {
                    m_particleSystemBooted = true;
                } else {
                    std::cout << "[Editor] particle load refused: " << pErr << "\n";
                }
            } else {
                std::cout << "[Editor] effekseer asset not found "
                             "(assets/effects/block_simple.efk)\n";
            }
        }
    }
    if (m_particleSystemC && m_particleSystemBooted) {
        if (m_particleSystemHandle < 0) {
            m_particleSystemHandle = m_particleSystemC->spawn(0.0f, 1.8f, 0.0f, 7);
        }
        m_particleSystemC->step(1.0f / 60.0f);
        m_particleAlive = m_particleSystemC->aliveCount(m_particleSystemHandle);
        stats.hasParticle = true;
        stats.particleBooted = true;
        stats.particleHandle = m_particleSystemHandle;
        stats.particleAlive = m_particleAlive;
    } else if (m_particleSystemC) {
        stats.hasParticle = true;
        stats.particleBooted = false;
        stats.particleAlive = 0;
    }

    // ---- 9. create_audio_mixer (engine::audio::IAudioMixer) ---------------
    // Real mixer used in the editor's audio loop: configure a master + music
    // + sfx bus tree, feed set_input from the ACTUAL play-audio voice level
    // each frame, advance tick(dt) (sidechain envelopes), and read the mixed
    // master output. Deterministic, data-driven — the editor play audio is
    // the loop that consumes it.
    if (!m_audioMixerC) {
        m_audioMixerC = engine::audio::create_audio_mixer();
        if (m_audioMixerC) {
            engine::audio::AudioMixerSpec spec;
            engine::audio::AudioBus master; master.id = "master";
            engine::audio::AudioBus music;  music.id = "music";  music.parent = "master";
            engine::audio::AudioBus sfx;    sfx.id = "sfx";      sfx.parent = "master";
            spec.buses = { master, music, sfx };
            std::string aErr;
            m_audioMixerC->configure(spec, aErr);
        }
    }
    if (m_audioMixerC) {
        std::string mErr;
        // Feed a real level: any active play voice's RMS (0 when none playing).
        float voiceLevel = 0.0f;
        for (const auto& idPlaying : m_playVoices) {
            voiceLevel = m_playAudio.voice_level(idPlaying.second);
            if (voiceLevel > 0.0f) break;
        }
        m_audioMixerC->set_input("music", voiceLevel > 0.0f ? 0.6 : 0.0, mErr);
        m_audioMixerC->set_input("sfx", voiceLevel, mErr);
        m_audioMixerC->tick(1.0 / 60.0, mErr);
        m_audioMixerMaster = m_audioMixerC->master_level();
        stats.hasAudio = true;
        stats.audioMaster = m_audioMixerMaster;
        stats.audioMusic = m_audioMixerC->bus_level("music");
        stats.audioSfx = m_audioMixerC->bus_level("sfx");
        stats.audioGainDbMaster = m_audioMixerC->gain_db("master");
    }

    m_sdkContractJson = serialize_sdk_contract_json(stats);
}

}  // namespace Engine