// IJobRunner.hpp
//
// Public cancellable job contracts for the procgen pipeline (META section 18
// / FALTANTES item 14: headless generation by cancellable jobs). The heavy
// procgen domains — WFC structures, erosion tile batches, mesh cooking
// batches — run as synchronous-but-cancellable units: each unit checks the
// cancellation token before starting, reports progress through a callback
// and stops early when cancelled, so generation never blocks the frame and
// can be aborted on shutdown/region unload. The caller decides the thread
// (the engine's ThreadPool or a project scheduler); this contract is the
// cancellable unit.
//
// Determinism: each unit is the same deterministic adapter the world uses, so
// the outputs of a completed batch are bit-identical to running the units
// individually, in any instance.
//
// This header is self-contained: it composes the public procgen contracts and
// never leaks a backend clone.

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "engine/procgen/IHeightmapErosion.hpp"
#include "engine/procgen/IMeshCooking.hpp"
#include "engine/procgen/IStructureGenerator.hpp"

namespace engine {
namespace procgen {

// Outcome of running a cancellable batch.
enum class JobResult {
    Completed,  // every unit ran
    Cancelled,  // a unit aborted on the cancellation token
    Failed      // a unit rejected its input (diagnostic in `error`)
};

// Progress of a running batch (completed/total units + human-readable phase).
struct JobProgress {
    std::size_t completed{ 0 };
    std::size_t total{ 0 };
    std::string phase;  // e.g. "erode tile 2/4", "structure 16x12"
    double fraction() const {
        return total == 0
                   ? 0.0
                   : static_cast<double>(completed) /
                         static_cast<double>(total);
    }
};

// A cooperative cancellation flag checked by batch jobs between units.
class ICancellationToken {
public:
    virtual ~ICancellationToken() = default;
    virtual bool cancelled() const = 0;
    virtual void cancel() = 0;
};

// Runs the heavy procgen domains as cancellable batches. All methods are
// synchronous (headless) and deterministic. `progress` (optional) fires
// before each unit with the completed-so-far count; a unit may call
// `token.cancel()` from inside the callback to abort the rest of the batch.
class IProcgenJobs {
public:
    virtual ~IProcgenJobs() = default;

    // Erodes `tiles` in order through the tile erosion cache. `outputs`
    // receives the eroded tiles in the same order as `tiles`; on Cancelled it
    // holds the prefix completed so far, on Failed the prefix before the
    // rejecting tile (diagnostic in `error`).
    virtual JobResult erode_tiles(
        const ErosionSpec& spec, const std::vector<Heightmap>& tiles,
        ICancellationToken& token,
        const std::function<void(const JobProgress&)>& progress,
        std::vector<Heightmap>& outputs, std::string& error) = 0;

    // Generates `sizes` WFC structure plans (derived seeds per size, like the
    // structure generator). `outputs` holds one StructureOutput per size.
    virtual JobResult generate_structures(
        const StructureAssetSpec& asset,
        const std::vector<std::pair<int, int>>& sizes,
        ICancellationToken& token,
        const std::function<void(const JobProgress&)>& progress,
        std::vector<StructureOutput>& outputs, std::string& error) = 0;

    // Cooks `meshes` under `options` (unwrap -> optimize -> optional
    // simplify). `outputs` holds one CookedMesh per input mesh.
    virtual JobResult cook_meshes(
        const CookOptions& options, const std::vector<CookedMesh>& meshes,
        ICancellationToken& token,
        const std::function<void(const JobProgress&)>& progress,
        std::vector<CookedMesh>& outputs, std::string& error) = 0;
};

// Factories (implemented by the SDK adapter — composes the existing procgen
// adapters; no new backend).
std::shared_ptr<ICancellationToken> create_cancellation_token();
std::shared_ptr<IProcgenJobs> create_procgen_jobs();

}  // namespace procgen
}  // namespace engine
