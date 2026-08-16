// ProcgenJobs.cpp
//
// SDK adapter for engine/procgen/IJobRunner.hpp (META section 18 /
// FALTANTES item 14: headless generation by cancellable jobs). This TU
// composes the existing deterministic procgen adapters — WFC structures,
// erosion (through the tile erosion cache) and mesh cooking — into
// cancellable batches: each unit checks the cancellation token before
// starting, reports progress through a callback and stops early when
// cancelled, so generation never blocks the frame and can be aborted on
// shutdown/region unload. The caller decides the thread; this TU is
// synchronous. Determinism: each unit is the same deterministic adapter the
// world uses, so a completed batch is bit-identical to running the units
// individually. No new backend.

#include "engine/procgen/IJobRunner.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace engine {
namespace procgen {
namespace {

class CancellationToken final : public ICancellationToken {
public:
    bool cancelled() const override {
        return flag_.load(std::memory_order_relaxed);
    }
    void cancel() override { flag_.store(true, std::memory_order_relaxed); }

private:
    std::atomic<bool> flag_{ false };
};

JobProgress make_progress(std::size_t completed, std::size_t total,
                          std::string phase) {
    JobProgress p;
    p.completed = completed;
    p.total = total;
    p.phase = std::move(phase);
    return p;
}

}  // namespace

class ProcgenJobs final : public IProcgenJobs {
public:
    JobResult erode_tiles(const ErosionSpec& spec,
                          const std::vector<Heightmap>& tiles,
                          ICancellationToken& token,
                          const std::function<void(const JobProgress&)>& progress,
                          std::vector<Heightmap>& outputs,
                          std::string& error) override {
        outputs.clear();
        if (tiles.empty()) {
            return JobResult::Completed;
        }
        auto erosion = create_heightmap_erosion();
        auto cache = create_tile_erosion_cache();
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            if (token.cancelled()) {
                return JobResult::Cancelled;
            }
            if (progress) {
                progress(make_progress(
                    i, tiles.size(),
                    "erode tile " + std::to_string(i + 1) + "/" +
                        std::to_string(tiles.size())));
            }
            Heightmap out;
            if (!cache->erode_tile(spec, static_cast<int>(i), 0, tiles[i], out,
                                   error)) {
                return JobResult::Failed;
            }
            outputs.push_back(std::move(out));
        }
        return JobResult::Completed;
    }

    JobResult generate_structures(
        const StructureAssetSpec& asset,
        const std::vector<std::pair<int, int>>& sizes,
        ICancellationToken& token,
        const std::function<void(const JobProgress&)>& progress,
        std::vector<StructureOutput>& outputs, std::string& error) override {
        outputs.clear();
        if (sizes.empty()) {
            return JobResult::Completed;
        }
        auto gen = create_structure_generator(asset, error);
        if (!gen) {
            return JobResult::Failed;
        }
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            if (token.cancelled()) {
                return JobResult::Cancelled;
            }
            const int w = sizes[i].first;
            const int h = sizes[i].second;
            if (progress) {
                progress(make_progress(
                    i, sizes.size(),
                    "structure " + std::to_string(w) + "x" +
                        std::to_string(h)));
            }
            StructureOutput so;
            if (!gen->generate(w, h, so, error)) {
                return JobResult::Failed;
            }
            outputs.push_back(std::move(so));
        }
        return JobResult::Completed;
    }

    JobResult cook_meshes(
        const CookOptions& options, const std::vector<CookedMesh>& meshes,
        ICancellationToken& token,
        const std::function<void(const JobProgress&)>& progress,
        std::vector<CookedMesh>& outputs, std::string& error) override {
        outputs.clear();
        if (meshes.empty()) {
            return JobResult::Completed;
        }
        auto cooker = create_mesh_cooker();
        for (std::size_t i = 0; i < meshes.size(); ++i) {
            if (token.cancelled()) {
                return JobResult::Cancelled;
            }
            if (progress) {
                progress(make_progress(
                    i, meshes.size(),
                    "cook mesh " + std::to_string(i + 1) + "/" +
                        std::to_string(meshes.size())));
            }
            CookedMesh out;
            CookStats stats;
            if (!cooker->cook(meshes[i], options, out, stats, error)) {
                return JobResult::Failed;
            }
            outputs.push_back(std::move(out));
        }
        return JobResult::Completed;
    }
};

}  // namespace procgen
}  // namespace engine

namespace engine {
namespace procgen {

std::shared_ptr<ICancellationToken> create_cancellation_token() {
    return std::make_shared<CancellationToken>();
}

std::shared_ptr<IProcgenJobs> create_procgen_jobs() {
    return std::make_shared<ProcgenJobs>();
}

}  // namespace procgen
}  // namespace engine
