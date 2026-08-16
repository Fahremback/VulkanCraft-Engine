// VoxelReplication.cpp
//
// SDK adapter for engine/voxel/IVoxelReplication.hpp (META section 17 /
// FALTANTES item 13). This is the ONLY translation unit that bridges the
// public transport-free replication contract to the generic networking
// runtime (engine/networking/NetworkRuntime.hpp): registered replication
// connections are real runtime connections and every outbound message
// (delta / rejection / chunk snapshot) flows through the runtime RpcQueue
// (reliable-ordered). The public contract never leaks networking types.
//
// Server authority: client block edits are validated (registered connection,
// per-connection cooldown, bounds, loaded chunk, world block registry via the
// transactional path) before they mutate the world; invalid edits never change
// state and are reported back so clients revert predictions. Accepted edits
// are broadcast as ordered deltas with a per-position monotonic revision.
//
// Client: optimistic break/place is applied locally and remembered; the
// authoritative result confirms or corrects it. Stale or duplicate messages
// (sequence or per-position revision) are dropped. Chunk snapshots stream by
// interest and are the reconnect/resync path.

#include "engine/voxel/IVoxelReplication.hpp"
#include "engine/voxel/IVoxelWorld.hpp"
#include "engine/voxel/IVoxelBlockEntity.hpp"
#include "engine/networking/NetworkRuntime.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine {
namespace voxel {
namespace {

constexpr std::uint32_t kRpcDelta = 1;
constexpr std::uint32_t kRpcReject = 2;
constexpr std::uint32_t kRpcChunk = 3;
constexpr int kWorldHeightLimit = 512;  // beyond the real world height
constexpr int kWorldRadius = 8192;      // horizontal bounds of the authority

int chunk_of(int v) {
    return v >= 0 ? v / kReplicationChunkSize
                  : -(((-v) + (kReplicationChunkSize - 1)) / kReplicationChunkSize);
}

struct Ivec3Hash {
    std::size_t operator()(const glm::ivec3& v) const noexcept {
        std::size_t h = std::hash<int>{}(v.x);
        h = h * 31 + std::hash<int>{}(v.y);
        h = h * 31 + std::hash<int>{}(v.z);
        return h;
    }
};
struct Ivec2Hash {
    std::size_t operator()(const glm::ivec2& v) const noexcept {
        std::size_t h = std::hash<int>{}(v.x);
        h = h * 31 + std::hash<int>{}(v.y);
        return h;
    }
};

// ---- little-endian helpers (std::string interops with the zstd provider) ----
void put_u32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
    }
}
void put_i32(std::string& out, int v) { put_u32(out, static_cast<std::uint32_t>(v)); }
bool get_u32(std::string_view& s, std::uint32_t& out) {
    if (s.size() < 4) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) {
        out |= static_cast<std::uint32_t>(static_cast<unsigned char>(s[i])) << (8 * i);
    }
    s.remove_prefix(4);
    return true;
}
bool get_i32(std::string_view& s, int& out) {
    std::uint32_t v;
    if (!get_u32(s, v)) return false;
    out = static_cast<int>(v);
    return true;
}
std::vector<std::byte> to_bytes(const std::string& s) {
    std::vector<std::byte> out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return out;
}

// ---- delta / position / snapshot / batch bodies ----
std::string encode_delta_body(const BlockReplicationDelta& d) {
    std::string out;
    out.reserve(28);
    put_i32(out, d.position.x);
    put_i32(out, d.position.y);
    put_i32(out, d.position.z);
    put_u32(out, d.blockId);
    put_u32(out, d.previousBlockId);
    put_u32(out, d.sequence);
    put_u32(out, d.revision);
    return out;
}
bool decode_delta_body(std::string_view& s, BlockReplicationDelta& out) {
    return get_i32(s, out.position.x) && get_i32(s, out.position.y) &&
           get_i32(s, out.position.z) && get_u32(s, out.blockId) &&
           get_u32(s, out.previousBlockId) && get_u32(s, out.sequence) &&
           get_u32(s, out.revision);
}
std::string encode_pos_body(const glm::ivec3& p) {
    std::string out;
    out.reserve(12);
    put_i32(out, p.x);
    put_i32(out, p.y);
    put_i32(out, p.z);
    return out;
}
bool decode_pos_body(std::string_view& s, glm::ivec3& out) {
    return get_i32(s, out.x) && get_i32(s, out.y) && get_i32(s, out.z);
}
std::string encode_snapshot_body(const ChunkReplicationSnapshot& s) {
    std::string out;
    put_i32(out, s.chunkX);
    put_i32(out, s.chunkZ);
    put_i32(out, s.minY);
    put_u32(out, s.height);
    put_u32(out, s.sequence);
    for (const std::uint32_t b : s.blocks) {
        put_u32(out, b);
    }
    return out;
}
bool decode_snapshot_body(std::string_view& s, ChunkReplicationSnapshot& out) {
    if (!get_i32(s, out.chunkX) || !get_i32(s, out.chunkZ) || !get_i32(s, out.minY) ||
        !get_u32(s, out.height) || !get_u32(s, out.sequence)) {
        return false;
    }
    const std::size_t count = static_cast<std::size_t>(kReplicationChunkSize) *
                              static_cast<std::size_t>(kReplicationChunkSize) * out.height;
    if (s.size() != count * 4) return false;
    out.blocks.clear();
    out.blocks.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t b;
        if (!get_u32(s, b)) return false;
        out.blocks.push_back(b);
    }
    return true;
}
std::string encode_batch_body(const ReplicationBatch& b) {
    std::string out;
    put_u32(out, b.sequence);
    put_u32(out, static_cast<std::uint32_t>(b.deltas.size()));
    put_u32(out, static_cast<std::uint32_t>(b.rejected.size()));
    for (const BlockReplicationDelta& d : b.deltas) {
        out += encode_delta_body(d);
    }
    for (const glm::ivec3& p : b.rejected) {
        out += encode_pos_body(p);
    }
    return out;
}
bool decode_batch_body(std::string_view& s, ReplicationBatch& out) {
    std::uint32_t deltas = 0;
    std::uint32_t rejected = 0;
    if (!get_u32(s, out.sequence) || !get_u32(s, deltas) || !get_u32(s, rejected)) {
        return false;
    }
    for (std::uint32_t i = 0; i < deltas; ++i) {
        BlockReplicationDelta d;
        if (!decode_delta_body(s, d)) return false;
        out.deltas.push_back(d);
    }
    for (std::uint32_t i = 0; i < rejected; ++i) {
        glm::ivec3 p;
        if (!decode_pos_body(s, p)) return false;
        out.rejected.push_back(p);
    }
    return true;
}

// ---- region body codec (region = chunks + block entities + fluid/light cells
// + entity snapshots; little-endian, same frame pattern as chunks/batches) ----
void put_f32(std::string& out, float v) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(v));
    std::memcpy(&bits, &v, sizeof(bits));
    put_u32(out, bits);
}
bool get_f32(std::string_view& s, float& out) {
    std::uint32_t bits;
    if (!get_u32(s, bits)) return false;
    std::memcpy(&out, &bits, sizeof(bits));
    return true;
}
void put_string(std::string& out, const std::string& s) {
    put_u32(out, static_cast<std::uint32_t>(s.size()));
    out += s;
}
bool get_string(std::string_view& s, std::string& out) {
    std::uint32_t len = 0;
    if (!get_u32(s, len)) return false;
    if (s.size() < len) return false;
    out.assign(s.data(), len);
    s.remove_prefix(len);
    return true;
}
void put_blob(std::string& out, const std::vector<std::uint8_t>& blob) {
    put_u32(out, static_cast<std::uint32_t>(blob.size()));
    for (const std::uint8_t b : blob) {
        out.push_back(static_cast<char>(b));
    }
}
bool get_blob(std::string_view& s, std::vector<std::uint8_t>& out) {
    std::uint32_t len = 0;
    if (!get_u32(s, len)) return false;
    if (s.size() < len) return false;
    out.assign(reinterpret_cast<const std::uint8_t*>(s.data()),
               reinterpret_cast<const std::uint8_t*>(s.data()) + len);
    s.remove_prefix(len);
    return true;
}
std::string encode_entity_body(const engine::entity::EntitySnapshot& e) {
    std::string out;
    put_string(out, e.type);
    put_f32(out, e.position.x);
    put_f32(out, e.position.y);
    put_f32(out, e.position.z);
    put_f32(out, e.health.value);
    put_f32(out, e.health.max);
    put_f32(out, e.tickInterval);
    put_u32(out, static_cast<std::uint32_t>(e.components.size()));
    for (const engine::entity::ComponentData& c : e.components) {
        put_string(out, c.type);
        put_u32(out, c.version);
        put_string(out, c.blob);
    }
    return out;
}
bool decode_entity_body(std::string_view& s, engine::entity::EntitySnapshot& out) {
    if (!get_string(s, out.type)) return false;
    if (!get_f32(s, out.position.x) || !get_f32(s, out.position.y) ||
        !get_f32(s, out.position.z)) {
        return false;
    }
    if (!get_f32(s, out.health.value) || !get_f32(s, out.health.max)) return false;
    if (!get_f32(s, out.tickInterval)) return false;
    std::uint32_t components = 0;
    if (!get_u32(s, components)) return false;
    out.components.clear();
    out.components.reserve(components);
    for (std::uint32_t i = 0; i < components; ++i) {
        engine::entity::ComponentData c;
        if (!get_string(s, c.type) || !get_u32(s, c.version) ||
            !get_string(s, c.blob)) {
            return false;
        }
        out.components.push_back(std::move(c));
    }
    return true;
}
std::string encode_region_body(const RegionReplicationSnapshot& r) {
    std::string out;
    put_u32(out, r.sequence);
    put_i32(out, r.origin.x);
    put_i32(out, r.origin.y);
    put_i32(out, r.origin.z);
    put_i32(out, r.chunkRadius);
    put_u32(out, static_cast<std::uint32_t>(r.chunks.size()));
    for (const ChunkReplicationSnapshot& c : r.chunks) {
        out += encode_snapshot_body(c);
    }
    put_u32(out, static_cast<std::uint32_t>(r.blockEntities.size()));
    for (const BlockEntityReplicationState& be : r.blockEntities) {
        put_i32(out, be.position.x);
        put_i32(out, be.position.y);
        put_i32(out, be.position.z);
        put_string(out, be.typeId);
        put_u32(out, be.dataVersion);
        put_blob(out, be.blob);
    }
    put_u32(out, static_cast<std::uint32_t>(r.cells.size()));
    for (const FluidLightReplicationCell& cell : r.cells) {
        put_i32(out, cell.position.x);
        put_i32(out, cell.position.y);
        put_i32(out, cell.position.z);
        out.push_back(static_cast<char>(cell.fluidLevel));
        out.push_back(static_cast<char>(cell.skyLight));
        out.push_back(static_cast<char>(cell.blockLight));
    }
    put_u32(out, static_cast<std::uint32_t>(r.entities.size()));
    for (const engine::entity::EntitySnapshot& e : r.entities) {
        out += encode_entity_body(e);
    }
    return out;
}
bool decode_region_body(std::string_view& s, RegionReplicationSnapshot& out) {
    if (!get_u32(s, out.sequence)) return false;
    if (!get_i32(s, out.origin.x) || !get_i32(s, out.origin.y) ||
        !get_i32(s, out.origin.z)) {
        return false;
    }
    if (!get_i32(s, out.chunkRadius)) return false;
    std::uint32_t chunks = 0;
    if (!get_u32(s, chunks)) return false;
    out.chunks.clear();
    out.chunks.reserve(chunks);
    for (std::uint32_t i = 0; i < chunks; ++i) {
        // Decode one chunk window: consume exactly its fields + block bytes
        // (decode_snapshot_body expects a view ending at the chunk, which is
        // not the case inside a region frame — decode inline here).
        ChunkReplicationSnapshot c;
        if (!get_i32(s, c.chunkX) || !get_i32(s, c.chunkZ) || !get_i32(s, c.minY) ||
            !get_u32(s, c.height) || !get_u32(s, c.sequence)) {
            return false;
        }
        const std::size_t count = static_cast<std::size_t>(kReplicationChunkSize) *
                                  static_cast<std::size_t>(kReplicationChunkSize) *
                                  c.height;
        if (s.size() < count * 4) return false;
        c.blocks.clear();
        c.blocks.reserve(count);
        for (std::size_t b = 0; b < count; ++b) {
            std::uint32_t blockId = 0;
            if (!get_u32(s, blockId)) return false;
            c.blocks.push_back(blockId);
        }
        out.chunks.push_back(std::move(c));
    }
    std::uint32_t blockEntities = 0;
    if (!get_u32(s, blockEntities)) return false;
    out.blockEntities.clear();
    out.blockEntities.reserve(blockEntities);
    for (std::uint32_t i = 0; i < blockEntities; ++i) {
        BlockEntityReplicationState be;
        if (!get_i32(s, be.position.x) || !get_i32(s, be.position.y) ||
            !get_i32(s, be.position.z)) {
            return false;
        }
        if (!get_string(s, be.typeId) || !get_u32(s, be.dataVersion) ||
            !get_blob(s, be.blob)) {
            return false;
        }
        out.blockEntities.push_back(std::move(be));
    }
    std::uint32_t cells = 0;
    if (!get_u32(s, cells)) return false;
    out.cells.clear();
    out.cells.reserve(cells);
    for (std::uint32_t i = 0; i < cells; ++i) {
        FluidLightReplicationCell cell;
        if (!get_i32(s, cell.position.x) || !get_i32(s, cell.position.y) ||
            !get_i32(s, cell.position.z)) {
            return false;
        }
        if (s.size() < 3) return false;
        cell.fluidLevel = static_cast<std::uint8_t>(s[0]);
        cell.skyLight = static_cast<std::uint8_t>(s[1]);
        cell.blockLight = static_cast<std::uint8_t>(s[2]);
        s.remove_prefix(3);
        out.cells.push_back(std::move(cell));
    }
    std::uint32_t entities = 0;
    if (!get_u32(s, entities)) return false;
    out.entities.clear();
    out.entities.reserve(entities);
    for (std::uint32_t i = 0; i < entities; ++i) {
        engine::entity::EntitySnapshot e;
        if (!decode_entity_body(s, e)) return false;
        out.entities.push_back(std::move(e));
    }
    return true;
}

// ---- codec frames (batching + optional zstd compression) ----
constexpr std::array<std::byte, 4> kBatchMagic{
    std::byte{'V'}, std::byte{'X'}, std::byte{'R'}, std::byte{'B'}};
constexpr std::array<std::byte, 4> kChunkMagic{
    std::byte{'V'}, std::byte{'X'}, std::byte{'R'}, std::byte{'C'}};
constexpr std::array<std::byte, 4> kRegionMagic{
    std::byte{'V'}, std::byte{'X'}, std::byte{'R'}, std::byte{'G'}};

std::vector<std::byte> frame(const std::array<std::byte, 4>& magic, const std::string& payload,
                             std::shared_ptr<const compression::ICompressionProvider> compression) {
    std::vector<std::byte> out;
    std::uint8_t flags = 0;
    std::string body = payload;
    if (compression) {
        const std::string compressed = compression->compress(payload);
        if (!compressed.empty()) {
            flags = 1;
            body = compressed;
        }
    }
    out.reserve(9 + body.size());
    out.insert(out.end(), magic.begin(), magic.end());
    out.push_back(std::byte{flags});
    // Length field stores the length of the BODY actually written (raw or
    // compressed), so deframe can validate the frame exactly.
    for (int i = 0; i < 4; ++i) {
        out.push_back(std::byte{(body.size() >> (8 * i)) & 0xFFu});
    }
    for (const char c : body) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return out;
}

bool deframe(const std::vector<std::byte>& data, const std::array<std::byte, 4>& magic,
             std::string& payload,
             std::shared_ptr<const compression::ICompressionProvider> compression) {
    if (data.size() < 9) return false;
    if (!std::equal(magic.begin(), magic.end(), data.begin())) return false;
    const std::uint8_t flags = static_cast<std::uint8_t>(data[4]);
    std::uint32_t len = 0;
    for (int i = 0; i < 4; ++i) {
        len |= static_cast<std::uint32_t>(data[5 + i]) << (8 * i);
    }
    if (data.size() != 9 + len) return false;
    payload.assign(reinterpret_cast<const char*>(data.data() + 9), len);
    if ((flags & 1u) != 0) {
        if (!compression) return false;
        payload = compression->decompress(payload);
        if (payload.empty()) return false;
    }
    return true;
}

Engine::Networking::RpcMessage make_message(std::uint32_t rpc,
                                            Engine::Networking::ConnectionId destination,
                                            std::string payload) {
    Engine::Networking::RpcMessage message;
    message.rpc = rpc;
    message.destination = destination;
    message.reliability = Engine::Networking::RpcReliability::ReliableOrdered;
    message.payload = to_bytes(payload);
    return message;
}

class VoxelReplication final : public IVoxelReplication {
public:
    explicit VoxelReplication(IVoxelWorld& world) : world_(world) {}

    const char* name() const override { return "authoritative"; }

    // ---- server (authority) ----

    void server_register_connection(ReplicationConnectionId connection) override {
        auto [it, inserted] = views_.try_emplace(connection);
        ConnectionView& view = it->second;
        if (inserted) {
            view.runtimeId = runtime_.connections().add(
                "voxel-replication-" + std::to_string(connection), 0);
            runtime_.connections().set_status(
                view.runtimeId, Engine::Networking::ConnectionStatus::Connected);
        }
        view.registered = true;
    }

    void server_unregister_connection(ReplicationConnectionId connection) override {
        auto it = views_.find(connection);
        if (it == views_.end()) return;
        it->second.registered = false;
        runtime_.connections().remove(it->second.runtimeId);
    }

    void server_set_interest(ReplicationConnectionId connection,
                             const ReplicationInterest& interest) override {
        auto it = views_.find(connection);
        if (it == views_.end()) return;
        it->second.interest = interest.position;
        it->second.radius = interest.chunkRadius;
    }

    ReplicationEditResult server_submit_edit(ReplicationConnectionId connection, int x, int y,
                                             int z, std::uint32_t blockId) override {
        ReplicationEditResult result;
        auto it = views_.find(connection);
        if (it == views_.end() || !it->second.registered) {
            result.error = "server: unknown replication connection";
            return result;
        }
        ConnectionView& view = it->second;
        const glm::ivec3 pos{x, y, z};
        if (tick_ < view.nextAllowedEditTick) {
            result.error = "server: edit cooldown (too many edits in a short window)";
            enqueue_reject(view, pos);
            return result;
        }
        view.nextAllowedEditTick = tick_ + kReplicationEditCooldownTicks;
        if (y < 0 || y >= kWorldHeightLimit || x < -kWorldRadius || x >= kWorldRadius ||
            z < -kWorldRadius || z >= kWorldRadius) {
            result.error = "server: edit out of bounds";
            enqueue_reject(view, pos);
            return result;
        }
        const glm::ivec2 chunk{chunk_of(x), chunk_of(z)};
        if (!world_.is_chunk_loaded(chunk.x, chunk.y)) {
            result.error = "server: chunk not loaded";
            enqueue_reject(view, pos);
            return result;
        }
        const std::uint32_t previous = world_.get_block(x, y, z);
        std::string error;
        auto tx = world_.begin_transaction();
        tx->set_block(x, y, z, blockId);
        if (!tx->commit(error)) {
            result.error = "server: rejected edit - " + error;
            enqueue_reject(view, pos);
            return result;
        }
        ++editCount_;
        const std::uint32_t now = world_.get_block(x, y, z);
        result.accepted = true;
        if (now == previous) {
            result.error = "server: no-op (block already set)";
            return result;  // accepted, but nothing changed so nothing to broadcast
        }
        // Single broadcast point (FALTANTES §7 item 137): the commit hook in
        // the world's transaction service already broadcast this edit through
        // server_broadcast_edits during tx->commit(); assign the revision it
        // produced so the submitting client gets the same monotonic value.
        result.revision = positionRevision_[pos];
        return result;
    }

    void server_broadcast_edits(const std::vector<BlockEdit>& edits) override {
        for (const BlockEdit& edit : edits) {
            if (edit.blockId == edit.previousBlockId) continue;  // no-op
            const glm::ivec3 pos = edit.position;
            const glm::ivec2 chunk{chunk_of(pos.x), chunk_of(pos.z)};
            const std::uint32_t revision = ++positionRevision_[pos];
            for (auto& [id, other] : views_) {
                if (!other.registered) continue;
                other.dirtyChunks.insert(chunk);
                BlockReplicationDelta delta{pos, edit.blockId, edit.previousBlockId,
                                            other.nextSequence++, revision};
                runtime_.rpcs().enqueue_outgoing(
                    make_message(kRpcDelta, other.runtimeId, encode_delta_body(delta)));
            }
        }
    }

    void server_update() override {
        ++tick_;
        // Stream newly-in-interest / dirty chunks to every registered connection,
        // then move each connection's runtime queue into its staged list.
        for (auto& [id, view] : views_) {
            if (!view.registered) continue;
            const glm::ivec2 center{chunk_of(view.interest.x), chunk_of(view.interest.z)};
            for (int dz = -view.radius; dz <= view.radius; ++dz) {
                for (int dx = -view.radius; dx <= view.radius; ++dx) {
                    const glm::ivec2 chunk{center.x + dx, center.y + dz};
                    if (!world_.is_chunk_loaded(chunk.x, chunk.y)) continue;
                    const bool sent = view.sentChunks.count(chunk) > 0;
                    const bool dirty = view.dirtyChunks.count(chunk) > 0;
                    if (sent && !dirty) continue;
                    view.sentChunks.insert(chunk);
                    view.dirtyChunks.erase(chunk);
                    ChunkReplicationSnapshot snapshot =
                        build_snapshot(chunk.x, chunk.y, view.nextSequence++);
                    runtime_.rpcs().enqueue_outgoing(
                        make_message(kRpcChunk, view.runtimeId, encode_snapshot_body(snapshot)));
                }
            }
            auto messages = runtime_.rpcs().drain_outgoing(view.runtimeId);
            for (auto& message : messages) {
                view.staged.push_back(std::move(message));
            }
        }
    }

    ReplicationBatch server_pack_batch(ReplicationConnectionId connection) override {
        ReplicationBatch batch;
        auto it = views_.find(connection);
        if (it == views_.end() || !it->second.registered) return batch;
        ConnectionView& view = it->second;
        std::deque<Engine::Networking::RpcMessage> keep;
        while (!view.staged.empty()) {
            auto message = std::move(view.staged.front());
            view.staged.pop_front();
            if (message.rpc == kRpcDelta) {
                std::string_view s(reinterpret_cast<const char*>(message.payload.data()),
                                   message.payload.size());
                BlockReplicationDelta delta;
                if (decode_delta_body(s, delta)) {
                    batch.deltas.push_back(delta);
                    batch.sequence = std::max(batch.sequence, delta.sequence);
                }
            } else if (message.rpc == kRpcReject) {
                std::string_view s(reinterpret_cast<const char*>(message.payload.data()),
                                   message.payload.size());
                glm::ivec3 rejected;
                if (decode_pos_body(s, rejected)) {
                    batch.rejected.push_back(rejected);
                }
            } else {
                keep.push_back(std::move(message));
            }
        }
        view.staged = std::move(keep);
        return batch;
    }

    std::vector<ChunkReplicationSnapshot> server_pack_interest(
        ReplicationConnectionId connection) override {
        std::vector<ChunkReplicationSnapshot> out;
        auto it = views_.find(connection);
        if (it == views_.end() || !it->second.registered) return out;
        ConnectionView& view = it->second;
        std::deque<Engine::Networking::RpcMessage> keep;
        while (!view.staged.empty()) {
            auto message = std::move(view.staged.front());
            view.staged.pop_front();
            if (message.rpc == kRpcChunk) {
                std::string_view s(reinterpret_cast<const char*>(message.payload.data()),
                                   message.payload.size());
                ChunkReplicationSnapshot snapshot;
                if (decode_snapshot_body(s, snapshot)) {
                    out.push_back(std::move(snapshot));
                }
            } else {
                keep.push_back(std::move(message));
            }
        }
        view.staged = std::move(keep);
        return out;
    }

    bool server_pack_region(ReplicationConnectionId connection,
                            RegionReplicationSnapshot& out,
                            std::string& errorOut) override {
        auto it = views_.find(connection);
        if (it == views_.end() || !it->second.registered) {
            errorOut = "server: unknown replication connection";
            return false;
        }
        ConnectionView& view = it->second;
        out.sequence = view.nextSequence++;
        out.origin = view.interest;
        out.chunkRadius = view.radius;
        const glm::ivec2 center{chunk_of(view.interest.x), chunk_of(view.interest.z)};
        // Deterministic traversal: the same world + interest always yields the
        // same region (dz/dx order matches server_update streaming).
        std::vector<glm::ivec2> regionChunks;
        for (int dz = -view.radius; dz <= view.radius; ++dz) {
            for (int dx = -view.radius; dx <= view.radius; ++dx) {
                const glm::ivec2 chunk{center.x + dx, center.y + dz};
                if (!world_.is_chunk_loaded(chunk.x, chunk.y)) continue;
                regionChunks.push_back(chunk);
                out.chunks.push_back(build_snapshot(chunk.x, chunk.y, out.sequence));
                // Block entities inside this chunk (deterministic position
                // order from the world).
                for (const auto& [position, entity] :
                     world_.block_entities_in_chunk(chunk.x, chunk.y)) {
                    BlockEntityReplicationState state;
                    state.position = position;
                    state.typeId = entity->type_id();
                    state.dataVersion = entity->data_version();
                    state.blob = entity->serialize_state();
                    out.blockEntities.push_back(std::move(state));
                }
                // Relevant fluid/light cells inside the snapshot window:
                // sparse — only cells where the layer differs from its default.
                const int baseX = chunk.x * kReplicationChunkSize;
                const int baseZ = chunk.y * kReplicationChunkSize;
                for (int y = snapshotMinY_; y < snapshotMinY_ + snapshotHeight_; ++y) {
                    for (int z = 0; z < kReplicationChunkSize; ++z) {
                        for (int x = 0; x < kReplicationChunkSize; ++x) {
                            const glm::ivec3 pos{baseX + x, y, baseZ + z};
                            const std::uint8_t fluid = world_.get_fluid_level(pos.x, pos.y, pos.z);
                            const std::uint8_t sky = world_.get_sky_light(pos.x, pos.y, pos.z);
                            const std::uint8_t block = world_.get_block_light(pos.x, pos.y, pos.z);
                            if (fluid == 0xFF && sky == 15 && block == 0) continue;
                            FluidLightReplicationCell cell;
                            cell.position = pos;
                            cell.fluidLevel = fluid;
                            cell.skyLight = sky;
                            cell.blockLight = block;
                            out.cells.push_back(std::move(cell));
                        }
                    }
                }
            }
        }
        // Entity snapshots whose position falls inside the region (the entity
        // layer's own deterministic serialization order).
        if (auto entityWorld = world_.entity_world()) {
            for (const engine::entity::EntitySnapshot& snapshot :
                 entityWorld->serialize_entities()) {
                const glm::ivec2 chunk{chunk_of(static_cast<int>(snapshot.position.x)),
                                       chunk_of(static_cast<int>(snapshot.position.z))};
                const bool inside =
                    std::find(regionChunks.begin(), regionChunks.end(), chunk) !=
                    regionChunks.end();
                if (inside) out.entities.push_back(snapshot);
            }
        }
        errorOut.clear();
        return true;
    }

    void server_set_snapshot_window(int minY, int height) override {
        if (height <= 0 || minY < -512 || minY + height > 1024) return;
        snapshotMinY_ = minY;
        snapshotHeight_ = height;
    }

    bool server_save(const std::string& filePath, std::string& errorOut) override {
        return world_.save_world(filePath, errorOut);
    }

    std::size_t server_edit_count() const override { return editCount_; }

    // ---- client ----

    bool client_predict(int x, int y, int z, std::uint32_t blockId) override {
        const glm::ivec3 pos{x, y, z};
        const std::uint32_t original = world_.get_block(x, y, z);
        if (original == blockId) return true;  // no-op: nothing to predict
        world_.set_block(x, y, z, blockId);
        predictions_[pos] = Prediction{original, blockId};
        return true;
    }

    void client_apply_batch(const ReplicationBatch& batch) override {
        // Server rejections revert pending predictions (restore the pre-edit block).
        for (const glm::ivec3& position : batch.rejected) {
            auto it = predictions_.find(position);
            if (it == predictions_.end()) continue;
            world_.set_block(position.x, position.y, position.z, it->second.originalBlock);
            predictions_.erase(it);
        }
        for (const BlockReplicationDelta& delta : batch.deltas) {
            if (delta.sequence <= lastAppliedSequence_) {
                ++staleDropped_;
                continue;
            }
            lastAppliedSequence_ = delta.sequence;
            const glm::ivec3 position = delta.position;
            auto rev = appliedRevision_.find(position);
            if (rev != appliedRevision_.end() && delta.revision <= rev->second) continue;
            appliedRevision_[position] = delta.revision;
            // Confirmed (authoritative == predicted) or corrected (authoritative
            // wins); either way the prediction is settled.
            auto predicted = predictions_.find(position);
            if (predicted != predictions_.end()) {
                predictions_.erase(predicted);
            }
            world_.set_block(position.x, position.y, position.z, delta.blockId);
        }
    }

    void client_apply_snapshot(const ChunkReplicationSnapshot& snapshot) override {
        if (snapshot.sequence <= lastAppliedSequence_) {
            ++staleDropped_;
            return;
        }
        lastAppliedSequence_ = snapshot.sequence;
        const std::size_t expected =
            static_cast<std::size_t>(kReplicationChunkSize) *
            static_cast<std::size_t>(kReplicationChunkSize) * snapshot.height;
        if (snapshot.blocks.size() != expected) return;  // malformed
        const int baseX = snapshot.chunkX * kReplicationChunkSize;
        const int baseZ = snapshot.chunkZ * kReplicationChunkSize;
        for (std::uint32_t y = 0; y < snapshot.height; ++y) {
            for (int z = 0; z < kReplicationChunkSize; ++z) {
                for (int x = 0; x < kReplicationChunkSize; ++x) {
                    const std::uint32_t id =
                        snapshot.blocks[(y * kReplicationChunkSize + z) *
                                            kReplicationChunkSize +
                                        x];
                    const glm::ivec3 position{baseX + x, snapshot.minY + static_cast<int>(y),
                                              baseZ + z};
                    if (world_.get_block(position.x, position.y, position.z) == id) continue;
                    world_.set_block(position.x, position.y, position.z, id);
                    predictions_.erase(position);  // authoritative snapshot wins
                }
            }
        }
    }

    bool client_apply_region(const RegionReplicationSnapshot& region,
                             std::string& errorOut) override {
        // All-or-nothing for block entities: every snapshot type must have a
        // registered factory before anything is mutated.
        for (const BlockEntityReplicationState& state : region.blockEntities) {
            std::string createError;
            auto entity = world_.create_block_entity(state.typeId, createError);
            if (!entity) {
                errorOut = "client: cannot reconstruct block entity '" +
                           state.typeId + "' - " + createError;
                return false;
            }
        }
        // Validate entity snapshots before mutating (same gates as load).
        for (const engine::entity::EntitySnapshot& snapshot : region.entities) {
            if (snapshot.type.empty()) {
                errorOut = "client: entity snapshot with empty type";
                return false;
            }
            if (snapshot.tickInterval < 0.0f) {
                errorOut = "client: entity '" + snapshot.type +
                           "': negative tick interval";
                return false;
            }
        }
        // 1) Blocks: every chunk window in the region.
        for (const ChunkReplicationSnapshot& chunk : region.chunks) {
            const std::size_t expected =
                static_cast<std::size_t>(kReplicationChunkSize) *
                static_cast<std::size_t>(kReplicationChunkSize) * chunk.height;
            if (chunk.blocks.size() != expected) {
                errorOut = "client: malformed region chunk window";
                return false;
            }
            const int baseX = chunk.chunkX * kReplicationChunkSize;
            const int baseZ = chunk.chunkZ * kReplicationChunkSize;
            for (std::uint32_t y = 0; y < chunk.height; ++y) {
                for (int z = 0; z < kReplicationChunkSize; ++z) {
                    for (int x = 0; x < kReplicationChunkSize; ++x) {
                        const std::uint32_t id =
                            chunk.blocks[(y * kReplicationChunkSize + z) *
                                            kReplicationChunkSize +
                                        x];
                        const glm::ivec3 position{baseX + x,
                                                  chunk.minY + static_cast<int>(y),
                                                  baseZ + z};
                        if (world_.get_block(position.x, position.y, position.z) == id) {
                            continue;
                        }
                        world_.set_block(position.x, position.y, position.z, id);
                        predictions_.erase(position);  // authoritative wins
                    }
                }
            }
        }
        // 2) Block entities: attach snapshot states (fresh instance from the
        // factory, blob deserialized); remove stale entities inside the region.
        std::vector<glm::ivec3> keep;
        for (const BlockEntityReplicationState& state : region.blockEntities) {
            keep.push_back(state.position);
            std::string createError;
            auto entity = world_.create_block_entity(state.typeId, createError);
            if (entity && entity->deserialize_state(state.blob, state.dataVersion)) {
                std::string attachError;
                world_.attach_block_entity(state.position.x, state.position.y,
                                           state.position.z, entity, attachError);
            }
        }
        // Remove stale block entities in the region (present locally, absent
        // from the snapshot).
        for (const ChunkReplicationSnapshot& chunk : region.chunks) {
            for (const auto& [position, entity] :
                 world_.block_entities_in_chunk(chunk.chunkX, chunk.chunkZ)) {
                (void)entity;
                const bool present = std::find(keep.begin(), keep.end(), position) !=
                                     keep.end();
                if (!present) world_.remove_block_entity(position.x, position.y, position.z);
            }
        }
        // 3) Entities: region reconcile — despawn region entities not in the
        // snapshot; spawn/apply snapshot entities (components, e.g. §14
        // inventory JSON blobs, ride as-is).
        auto entityWorld = world_.entity_world();
        if (entityWorld) {
            for (const ChunkReplicationSnapshot& chunk : region.chunks) {
                const auto current =
                    entityWorld->entities_in_chunk(chunk.chunkX, chunk.chunkZ);
                for (const engine::entity::EntityId& handle : current) {
                    engine::entity::Position local;
                    if (!entityWorld->get_position(handle, local)) continue;
                    const bool inSnapshot =
                        std::any_of(region.entities.begin(), region.entities.end(),
                                    [&](const engine::entity::EntitySnapshot& s) {
                                        return s.type == entityWorld->type_of(handle) &&
                                               s.position.x == local.x &&
                                               s.position.y == local.y &&
                                               s.position.z == local.z;
                                    });
                    if (!inSnapshot) entityWorld->despawn(handle);
                }
            }
            for (const engine::entity::EntitySnapshot& snapshot : region.entities) {
                const glm::ivec2 chunk{chunk_of(static_cast<int>(snapshot.position.x)),
                                       chunk_of(static_cast<int>(snapshot.position.z))};
                const bool inRegion =
                    std::any_of(region.chunks.begin(), region.chunks.end(),
                                [&](const ChunkReplicationSnapshot& c) {
                                    return c.chunkX == chunk.x && c.chunkZ == chunk.y;
                                });
                if (!inRegion) continue;
                std::string spawnError;
                const engine::entity::EntityId handle =
                    entityWorld->spawn(snapshot.type, snapshot.position, spawnError);
                if (!handle.valid()) continue;
                entityWorld->set_health(handle, snapshot.health);
                if (snapshot.tickInterval > 0.0f) {
                    entityWorld->set_tick_interval(handle, snapshot.tickInterval);
                }
                for (const engine::entity::ComponentData& component :
                     snapshot.components) {
                    entityWorld->set_component(handle, component);
                }
            }
        }
        lastAppliedSequence_ = region.sequence;
        errorOut.clear();
        return true;
    }

    std::uint32_t client_applied_sequence() const override { return lastAppliedSequence_; }
    std::size_t client_pending_predictions() const override { return predictions_.size(); }
    std::size_t client_stale_dropped() const override { return staleDropped_; }

private:
    struct ConnectionView {
        Engine::Networking::ConnectionId runtimeId{};
        bool registered{false};
        glm::ivec3 interest{0, 0, 0};
        int radius{0};
        std::uint32_t nextSequence{1};
        std::uint64_t nextAllowedEditTick{0};
        std::deque<Engine::Networking::RpcMessage> staged;
        std::unordered_set<glm::ivec2, Ivec2Hash> sentChunks;
        std::unordered_set<glm::ivec2, Ivec2Hash> dirtyChunks;
    };
    struct Prediction {
        std::uint32_t originalBlock;
        std::uint32_t predictedBlock;
    };

    void enqueue_reject(ConnectionView& view, const glm::ivec3& position) {
        runtime_.rpcs().enqueue_outgoing(
            make_message(kRpcReject, view.runtimeId, encode_pos_body(position)));
    }

    ChunkReplicationSnapshot build_snapshot(int chunkX, int chunkZ,
                                            std::uint32_t sequence) const {
        ChunkReplicationSnapshot snapshot;
        snapshot.chunkX = chunkX;
        snapshot.chunkZ = chunkZ;
        snapshot.minY = snapshotMinY_;
        snapshot.height = static_cast<std::uint32_t>(snapshotHeight_);
        snapshot.sequence = sequence;
        const int baseX = chunkX * kReplicationChunkSize;
        const int baseZ = chunkZ * kReplicationChunkSize;
        snapshot.blocks.reserve(static_cast<std::size_t>(kReplicationChunkSize) *
                                static_cast<std::size_t>(kReplicationChunkSize) *
                                snapshot.height);
        for (int y = 0; y < snapshotHeight_; ++y) {
            for (int z = 0; z < kReplicationChunkSize; ++z) {
                for (int x = 0; x < kReplicationChunkSize; ++x) {
                    snapshot.blocks.push_back(
                        world_.get_block(baseX + x, snapshotMinY_ + y, baseZ + z));
                }
            }
        }
        return snapshot;
    }

    IVoxelWorld& world_;
    Engine::Networking::NetworkingRuntime runtime_;
    std::unordered_map<ReplicationConnectionId, ConnectionView> views_;
    std::unordered_map<glm::ivec3, std::uint32_t, Ivec3Hash> positionRevision_;
    std::uint64_t tick_{0};
    std::size_t editCount_{0};
    int snapshotMinY_{kReplicationDefaultMinY};
    int snapshotHeight_{kReplicationDefaultHeight};
    // Client state (one adapter instance per client world/role).
    std::uint32_t lastAppliedSequence_{0};
    std::unordered_map<glm::ivec3, std::uint32_t, Ivec3Hash> appliedRevision_;
    std::unordered_map<glm::ivec3, Prediction, Ivec3Hash> predictions_;
    std::size_t staleDropped_{0};
};

}  // namespace

// ---- public codec ----

std::vector<std::byte> encode_replication_batch(
    const ReplicationBatch& batch,
    std::shared_ptr<const compression::ICompressionProvider> compression) {
    return frame(kBatchMagic, encode_batch_body(batch), compression);
}

bool decode_replication_batch(const std::vector<std::byte>& data, ReplicationBatch& out,
                              std::shared_ptr<const compression::ICompressionProvider> compression) {
    std::string payload;
    if (!deframe(data, kBatchMagic, payload, compression)) return false;
    std::string_view s(payload);
    return decode_batch_body(s, out);
}

std::vector<std::byte> encode_replication_snapshot(
    const ChunkReplicationSnapshot& snapshot,
    std::shared_ptr<const compression::ICompressionProvider> compression) {
    return frame(kChunkMagic, encode_snapshot_body(snapshot), compression);
}

bool decode_replication_snapshot(const std::vector<std::byte>& data,
                                 ChunkReplicationSnapshot& out,
                                 std::shared_ptr<const compression::ICompressionProvider> compression) {
    std::string payload;
    if (!deframe(data, kChunkMagic, payload, compression)) return false;
    std::string_view s(payload);
    return decode_snapshot_body(s, out);
}

std::vector<std::byte> encode_replication_region(
    const RegionReplicationSnapshot& region,
    std::shared_ptr<const compression::ICompressionProvider> compression) {
    return frame(kRegionMagic, encode_region_body(region), compression);
}

bool decode_replication_region(const std::vector<std::byte>& data,
                               RegionReplicationSnapshot& out,
                               std::shared_ptr<const compression::ICompressionProvider> compression) {
    std::string payload;
    if (!deframe(data, kRegionMagic, payload, compression)) return false;
    std::string_view s(payload);
    return decode_region_body(s, out);
}

std::shared_ptr<IVoxelReplication> create_voxel_replication(IVoxelWorld& world) {
    auto replication = std::make_shared<VoxelReplication>(world);
    // FALTANTES §7 item 137: the adapter is wired to the world at creation so
    // the transaction commit hook can broadcast (the world's single mutation
    // authority notifies replication on every successful commit). A world
    // without an adapter gets no broadcast; re-registering replaces the
    // adapter (the previous one's views stay orphaned but harmless).
    world.register_replication(replication);
    return replication;
}

}  // namespace voxel
}  // namespace engine
