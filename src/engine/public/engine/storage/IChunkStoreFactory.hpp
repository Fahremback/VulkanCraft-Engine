// IChunkStoreFactory — public factory for the RocksDB-backed chunk store.
//
// Promoted solution (META section 32): RocksDB from external/solutions/rocksdb
// is the "chunks/event log" persistence authority (DEPENDENCY_POLICY). This
// header only declares the factory; the implementation lives in the SDK module
// where the rocksdb headers stay private. Self-contained: safe for any TU.
#pragma once

#include <memory>
#include <string>

namespace engine {
namespace voxel {

class IChunkStorage;

}  // namespace voxel

namespace storage {

// Builds a chunk store that persists world-save blobs inside a RocksDB
// database (BLAKE3 content-addressed keys, zstd frames as values). Implements
// the public IChunkStorage contract, so a world delegates save/load to it via
// IVoxelWorld::register_storage. The rocksdb headers never appear here.
std::shared_ptr<engine::voxel::IChunkStorage> create_rocksdb_chunk_storage();

}  // namespace storage
}  // namespace engine
