#pragma once

// Block-entity scripting bridge (task_plan B.2 — the engine-side scripting
// leg). Block entities may declare a project-owned script through
// IVoxelBlockEntity::script_id(); this contract is the runtime that actually
// RUNS those scripts through the engine's authoritative script compiler/VM.
//
// Ownership split: the engine owns the VM (engine/scripting); the project
// owns the script CONTENT (authored as JSON .script documents — the same
// schema ScriptGraphAsset::load parses) and the entity behavior. The engine
// only routes: register -> compile -> run per attached entity, in
// deterministic position order.
//
// This contract is also the engine-side half of the editor/inspector + MCP
// integration: the inspector reads live script variables through
// script_variable() (no UI here — the editor consumes this seam), and the MCP
// authors the JSON documents register_script() consumes.
//
// The bridge takes over the world's block-entity listener slot for its
// lifetime (Attached/Detached events keep the instance table in sync with the
// world); it releases the slot on destruction.

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace engine {
namespace voxel {

class IVoxelWorld;

// Definition of one block-entity script. scriptId must match the value
// IVoxelBlockEntity::script_id() returns on the entities it should drive.
// graphJson is a JSON document in the .script graph format.
//
// Runtime contract: the bridge fires the one-shot "init" event when an
// instance first loads (scripts that declare no "init" event are untouched),
// then "on_tick" on every tick while the instance is idle/completed. A
// Waiting script keeps waiting across ticks and is never re-entered
// mid-flight. Per-instance variables are seeded before "init": x, y, z
// (block position) and scriptId.
struct BlockEntityScriptSpec {
    std::string scriptId;   // namespaced id, e.g. "project:chest_loot"
    std::string graphJson;  // .script JSON document
    uint32_t version{ 1 };  // project-owned format version
};

// Runtime bridge that runs block entities' declared scripts.
class IBlockEntityScripting {
public:
    virtual ~IBlockEntityScripting() = default;

    // Compiles and registers a script. Returns false (with last_error() set)
    // on invalid JSON or a compile failure. Re-registering replaces the program.
    virtual bool register_script(const BlockEntityScriptSpec& spec) = 0;

    // Removes a registered script. True when it was registered.
    virtual bool unregister_script(const std::string& scriptId) = 0;
    virtual bool has_script(const std::string& scriptId) const = 0;

    // Advances every attached entity whose script_id() is registered, in
    // deterministic block-position order. dt is seconds.
    virtual void tick(double dt) = 0;

    // Observability (inspector/debugger seam).
    virtual std::size_t active_instances() const = 0;
    virtual std::uint64_t completed_runs() const = 0;
    virtual std::uint64_t failed_runs() const = 0;
    virtual std::string last_error() const = 0;

    // Reads a live variable from the script instance at a block position
    // (editor inspector consumer). Returns false when no running instance
    // owns that position or the variable does not exist / is not numeric.
    virtual bool script_variable(const glm::ivec3& position,
                                 const std::string& name,
                                 double& out) const = 0;
};

// Factory: creates the bridge bound to `world`. The bridge subscribes to the
// world's block-entity listener and releases it on destruction.
std::unique_ptr<IBlockEntityScripting> create_block_entity_scripting(
    IVoxelWorld& world);

}  // namespace voxel
}  // namespace engine
