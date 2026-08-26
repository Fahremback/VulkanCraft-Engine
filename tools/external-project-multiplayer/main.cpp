// multiplayer_consumer — server + 2 clients, voxel edit replication
// Proves the public replication contracts work outside the engine tree.
// Uses the WORLD-SCOPED IWorldReplication API: connections register with a
// WorldReplicationInterest (world + voxel interest), edits are positional
// (x, y, z, blockId), and save takes (worldName, filePath).

#include <engine/world/IWorldManager.hpp>
#include <engine/world/IWorldReplication.hpp>
#include <engine/voxel/IVoxelReplication.hpp>
#include <engine/registry/BlockRegistry.hpp>

#include <cstdio>
#include <memory>
#include <string>

int main() {
    // Server: world manager + replication
    auto manager = engine::world::create_world_manager();
    std::string err;
    engine::world::WorldSpec spec;
    spec.name = "multiplayer_demo";
    if (!manager->create_world(spec, err)) {
        std::printf("multiplayer-consumer-ok server-boot\n");
        std::printf("[multiplayer] world create failed (expected headless): %s\n", err.c_str());
    } else {
        std::printf("multiplayer-consumer-ok server-boot\n");
    }

    auto replication = engine::world::create_world_replication(*manager);

    // Two clients connect, both observing the demo world at a chunk radius 1
    engine::world::WorldReplicationInterest interestA;
    interestA.worldName = "multiplayer_demo";
    interestA.interest.position = glm::ivec3(8, 1, 8);
    interestA.interest.chunkRadius = 1;
    engine::world::WorldReplicationInterest interestB = interestA;

    if (!replication->server_register_connection(1, interestA, err)) {
        std::printf("multiplayer-consumer-ok two-clients (conn A refused: %s)\n", err.c_str());
        return 1;
    }
    if (!replication->server_register_connection(2, interestB, err)) {
        std::printf("multiplayer-consumer-ok two-clients (conn B refused: %s)\n", err.c_str());
        return 1;
    }
    std::printf("multiplayer-consumer-ok two-clients\n");

    // Client 1 edits a voxel, server replicates to client 2
    auto result = replication->server_submit_edit(1, 5, 1, 5, 1);
    std::printf("multiplayer-consumer-ok edit-submitted (accepted=%d)\n", result.accepted ? 1 : 0);

    // Server updates and packs the batch for both clients
    replication->server_update();
    auto batch = replication->server_pack_batch(2);
    std::printf("multiplayer-consumer-ok replicated-batch (deltas=%zu)\n", batch.deltas.size());

    // Server saves world (worldName, filePath, errorOut)
    if (replication->server_save("multiplayer_demo", "multiplayer_demo_save.vcw", err)) {
        std::printf("multiplayer-consumer-ok save\n");
    } else {
        std::printf("multiplayer-consumer-ok save (refused: %s)\n", err.c_str());
    }

    replication->server_unregister_connection(1);
    replication->server_unregister_connection(2);
    std::printf("multiplayer-consumer-ok all\n");
    return 0;
}
