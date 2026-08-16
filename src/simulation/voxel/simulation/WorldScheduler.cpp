#include "WorldScheduler.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <utility>

namespace {
// Deterministic coordinate ordering shared by every phase: (x, y, z) ascending.
bool coord_less(const TickCell& a, const TickCell& b) {
    return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
}

void put_u32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
    }
}
void put_i32(std::vector<std::byte>& out, int v) {
    put_u32(out, static_cast<std::uint32_t>(v));
}
void put_u64(std::vector<std::byte>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
    }
}
void put_f32(std::vector<std::byte>& out, float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u32(out, bits);
}
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
bool get_u64(std::string_view& s, std::uint64_t& out) {
    if (s.size() < 8) return false;
    out = 0;
    for (int i = 0; i < 8; ++i) {
        out |= static_cast<std::uint64_t>(static_cast<unsigned char>(s[i])) << (8 * i);
    }
    s.remove_prefix(8);
    return true;
}
bool get_f32(std::string_view& s, float& out) {
    std::uint32_t bits;
    if (!get_u32(s, bits)) return false;
    std::memcpy(&out, &bits, sizeof(bits));
    return true;
}
bool get_cell(std::string_view& s, TickCell& cell) {
    return get_i32(s, cell.x) && get_i32(s, cell.y) && get_i32(s, cell.z);
}
void put_cell(std::vector<std::byte>& out, const TickCell& cell) {
    put_i32(out, cell.x);
    put_i32(out, cell.y);
    put_i32(out, cell.z);
}
}  // namespace

WorldScheduler::WorldScheduler(uint64_t seed, int chunkSize, int randomHeight)
    : seed_(seed),
      chunkSize_(chunkSize > 0 ? chunkSize : 16),
      randomHeight_(randomHeight > 0 ? randomHeight : 128) {}

int WorldScheduler::advance(float deltaSeconds, float fixedStepSeconds) {
    if (fixedStepSeconds <= 0.0f || deltaSeconds <= 0.0f) return 0;
    accumulator_ += deltaSeconds;
    int ticks = 0;
    while (accumulator_ >= fixedStepSeconds && ticks < maxTicksPerAdvance_) {
        accumulator_ -= fixedStepSeconds;
        run_tick();
        ++tick_;
        ++ticks;
        if (tickCallback_) tickCallback_(tick_);
    }
    return ticks;
}

void WorldScheduler::schedule_block_tick(TickCell cell, int priority) {
    blockPending_[cell] = priority;
}

void WorldScheduler::schedule_random_tick(int chunkX, int chunkZ) {
    randomChunks_.insert(TickChunk{ chunkX, chunkZ });
}

void WorldScheduler::schedule_fluid_tick(TickCell cell) {
    fluidPending_.insert(cell);
}

void WorldScheduler::schedule_scheduled_tick(TickCell cell, uint64_t atTick) {
    scheduledPending_[cell] = atTick;
}

void WorldScheduler::schedule_neighbor_tick(TickCell cell) {
    neighborCenters_.insert(cell);
}

void WorldScheduler::cancel_cell(TickCell cell) {
    blockPending_.erase(cell);
    fluidPending_.erase(cell);
    scheduledPending_.erase(cell);
    neighborCenters_.erase(cell);
}

void WorldScheduler::cancel_neighbor(TickCell cell) {
    neighborCenters_.erase(cell);
}

void WorldScheduler::cancel_chunk(int chunkX, int chunkZ) {
    randomChunks_.erase(TickChunk{ chunkX, chunkZ });
    const int minX = chunkX * chunkSize_;
    const int minZ = chunkZ * chunkSize_;
    const int maxX = minX + chunkSize_;
    const int maxZ = minZ + chunkSize_;
    const auto in_chunk = [&](const TickCell& cell) {
        return cell.x >= minX && cell.x < maxX &&
               cell.z >= minZ && cell.z < maxZ;
    };
    for (auto it = blockPending_.begin(); it != blockPending_.end();) {
        if (in_chunk(it->first)) it = blockPending_.erase(it);
        else ++it;
    }
    for (auto it = fluidPending_.begin(); it != fluidPending_.end();) {
        if (in_chunk(*it)) it = fluidPending_.erase(it);
        else ++it;
    }
    for (auto it = scheduledPending_.begin(); it != scheduledPending_.end();) {
        if (in_chunk(it->first)) it = scheduledPending_.erase(it);
        else ++it;
    }
    for (auto it = neighborCenters_.begin(); it != neighborCenters_.end();) {
        if (in_chunk(*it)) it = neighborCenters_.erase(it);
        else ++it;
    }
}

void WorldScheduler::set_handler(Phase phase, Handler handler) {
    handlers_[static_cast<int>(phase)] = std::move(handler);
}

void WorldScheduler::set_neighbor_handler(NeighborHandler handler) {
    neighborHandler_ = std::move(handler);
}

void WorldScheduler::set_budget(Phase phase, int maxPerTick) {
    budgets_[static_cast<int>(phase)] = maxPerTick;
}

void WorldScheduler::set_tick_callback(std::function<void(uint64_t)> callback) {
    tickCallback_ = std::move(callback);
}

void WorldScheduler::set_active_center(int chunkX, int chunkZ) {
    activeCenterX_ = chunkX;
    activeCenterZ_ = chunkZ;
}

void WorldScheduler::set_active_radius(int chunks) {
    activeRadius_ = chunks;
}

std::size_t WorldScheduler::pending_count() const {
    return blockPending_.size() + randomChunks_.size() +
           fluidPending_.size() + scheduledPending_.size() +
           neighborCenters_.size();
}

WorldScheduler::State WorldScheduler::capture_state() const {
    State state;
    state.tick = tick_;
    state.accumulator = accumulator_;
    state.block.reserve(blockPending_.size());
    for (const auto& [cell, priority] : blockPending_) {
        state.block.emplace_back(cell, priority);
    }
    std::sort(state.block.begin(), state.block.end(),
              [](const std::pair<TickCell, int>& a,
                 const std::pair<TickCell, int>& b) {
        if (a.second != b.second) return a.second > b.second;  // priority desc
        return coord_less(a.first, b.first);
    });
    state.randomChunks.assign(randomChunks_.begin(), randomChunks_.end());
    std::sort(state.randomChunks.begin(), state.randomChunks.end(),
              [](const TickChunk& a, const TickChunk& b) {
        return std::tie(a.x, a.z) < std::tie(b.x, b.z);
    });
    state.fluid.assign(fluidPending_.begin(), fluidPending_.end());
    std::sort(state.fluid.begin(), state.fluid.end(), coord_less);
    state.scheduled.reserve(scheduledPending_.size());
    for (const auto& [cell, deadline] : scheduledPending_) {
        state.scheduled.emplace_back(cell, deadline);
    }
    std::sort(state.scheduled.begin(), state.scheduled.end(),
              [](const std::pair<TickCell, uint64_t>& a,
                 const std::pair<TickCell, uint64_t>& b) {
        if (a.second != b.second) return a.second < b.second;  // deadline asc
        return coord_less(a.first, b.first);
    });
    state.neighbors.assign(neighborCenters_.begin(), neighborCenters_.end());
    std::sort(state.neighbors.begin(), state.neighbors.end(), coord_less);
    return state;
}

void WorldScheduler::restore_state(const State& state) {
    tick_ = state.tick;
    accumulator_ = state.accumulator;
    blockPending_.clear();
    for (const auto& [cell, priority] : state.block) blockPending_[cell] = priority;
    randomChunks_.clear();
    for (const TickChunk& chunk : state.randomChunks) randomChunks_.insert(chunk);
    fluidPending_.clear();
    for (const TickCell& cell : state.fluid) fluidPending_.insert(cell);
    scheduledPending_.clear();
    for (const auto& [cell, deadline] : state.scheduled) scheduledPending_[cell] = deadline;
    neighborCenters_.clear();
    for (const TickCell& cell : state.neighbors) neighborCenters_.insert(cell);
}

std::vector<std::byte> WorldScheduler::serialize_state() const {
    const State state = capture_state();
    std::vector<std::byte> out;
    out.reserve(64 + state.block.size() * 16 + state.randomChunks.size() * 8 +
                state.fluid.size() * 12 + state.scheduled.size() * 20 +
                state.neighbors.size() * 12);
    for (char c : std::string("VCWS")) out.push_back(static_cast<std::byte>(c));
    put_u32(out, 1);  // schema version
    put_u64(out, state.tick);
    put_f32(out, state.accumulator);
    put_u32(out, static_cast<std::uint32_t>(state.block.size()));
    for (const auto& [cell, priority] : state.block) {
        put_cell(out, cell);
        put_i32(out, priority);
    }
    put_u32(out, static_cast<std::uint32_t>(state.randomChunks.size()));
    for (const TickChunk& chunk : state.randomChunks) {
        put_i32(out, chunk.x);
        put_i32(out, chunk.z);
    }
    put_u32(out, static_cast<std::uint32_t>(state.fluid.size()));
    for (const TickCell& cell : state.fluid) put_cell(out, cell);
    put_u32(out, static_cast<std::uint32_t>(state.scheduled.size()));
    for (const auto& [cell, deadline] : state.scheduled) {
        put_cell(out, cell);
        put_u64(out, deadline);
    }
    put_u32(out, static_cast<std::uint32_t>(state.neighbors.size()));
    for (const TickCell& cell : state.neighbors) put_cell(out, cell);
    return out;
}

bool WorldScheduler::deserialize_state(const std::vector<std::byte>& data,
                                       std::string& errorOut) {
    if (data.size() < 4 + 4 + 8 + 4) {
        errorOut = "scheduler state too small";
        return false;
    }
    std::string_view s(reinterpret_cast<const char*>(data.data()), data.size());
    if (s.size() < 4 || s.substr(0, 4) != std::string_view("VCWS")) {
        errorOut = "scheduler state: bad magic";
        return false;
    }
    s.remove_prefix(4);
    std::uint32_t version = 0;
    if (!get_u32(s, version) || version != 1) {
        errorOut = "scheduler state: unsupported version " + std::to_string(version);
        return false;
    }
    State state;
    if (!get_u64(s, state.tick) || !get_f32(s, state.accumulator)) {
        errorOut = "scheduler state: truncated clock";
        return false;
    }
    std::uint32_t count = 0;
    if (!get_u32(s, count)) { errorOut = "scheduler state: truncated (block)"; return false; }
    for (std::uint32_t i = 0; i < count; ++i) {
        TickCell cell;
        int priority = 0;
        if (!get_cell(s, cell) || !get_i32(s, priority)) {
            errorOut = "scheduler state: truncated (block " + std::to_string(i) + ")";
            return false;
        }
        state.block.emplace_back(cell, priority);
    }
    if (!get_u32(s, count)) { errorOut = "scheduler state: truncated (random)"; return false; }
    for (std::uint32_t i = 0; i < count; ++i) {
        TickChunk chunk;
        if (!get_i32(s, chunk.x) || !get_i32(s, chunk.z)) {
            errorOut = "scheduler state: truncated (random " + std::to_string(i) + ")";
            return false;
        }
        state.randomChunks.push_back(chunk);
    }
    if (!get_u32(s, count)) { errorOut = "scheduler state: truncated (fluid)"; return false; }
    for (std::uint32_t i = 0; i < count; ++i) {
        TickCell cell;
        if (!get_cell(s, cell)) {
            errorOut = "scheduler state: truncated (fluid " + std::to_string(i) + ")";
            return false;
        }
        state.fluid.push_back(cell);
    }
    if (!get_u32(s, count)) { errorOut = "scheduler state: truncated (scheduled)"; return false; }
    for (std::uint32_t i = 0; i < count; ++i) {
        TickCell cell;
        std::uint64_t deadline = 0;
        if (!get_cell(s, cell) || !get_u64(s, deadline)) {
            errorOut = "scheduler state: truncated (scheduled " + std::to_string(i) + ")";
            return false;
        }
        state.scheduled.emplace_back(cell, deadline);
    }
    if (!get_u32(s, count)) { errorOut = "scheduler state: truncated (neighbors)"; return false; }
    for (std::uint32_t i = 0; i < count; ++i) {
        TickCell cell;
        if (!get_cell(s, cell)) {
            errorOut = "scheduler state: truncated (neighbor " + std::to_string(i) + ")";
            return false;
        }
        state.neighbors.push_back(cell);
    }
    restore_state(state);
    return true;
}

void WorldScheduler::run_tick() {
    run_phase(Phase::BlockTick);
    run_random_phase();
    run_phase(Phase::FluidTick);
    run_scheduled_phase();
    run_neighbor_phase();
}

void WorldScheduler::run_phase(Phase phase) {
    const int index = static_cast<int>(phase);
    const int budget = budgets_[index];
    std::vector<TickCell> cells;
    if (phase == Phase::BlockTick) {
        cells.reserve(blockPending_.size());
        for (const auto& entry : blockPending_) cells.push_back(entry.first);
        // Higher priority first; ties and everything else in (x, y, z).
        std::sort(cells.begin(), cells.end(), [&](const TickCell& a, const TickCell& b) {
            const int pa = blockPending_.at(a);
            const int pb = blockPending_.at(b);
            if (pa != pb) return pa > pb;
            return coord_less(a, b);
        });
    } else {
        cells.reserve(fluidPending_.size());
        for (const TickCell& cell : fluidPending_) cells.push_back(cell);
        std::sort(cells.begin(), cells.end(), coord_less);
    }

    int ran = 0;
    for (const TickCell& cell : cells) {
        // Sleeping cells never consume the budget: they wake and execute when
        // the active center approaches.
        if (!is_active(cell)) continue;
        if (budget >= 0 && ran >= budget) break;
        if (phase == Phase::BlockTick) blockPending_.erase(cell);
        else fluidPending_.erase(cell);
        if (handlers_[index]) handlers_[index](cell);
        ++executed_[index];
        ++ran;
    }
}

void WorldScheduler::run_random_phase() {
    const int index = static_cast<int>(Phase::RandomTick);
    if (randomChunks_.empty()) return;
    const int budget = budgets_[index];
    std::vector<TickChunk> chunks(randomChunks_.begin(), randomChunks_.end());
    std::sort(chunks.begin(), chunks.end(), [](const TickChunk& a, const TickChunk& b) {
        return std::tie(a.x, a.z) < std::tie(b.x, b.z);
    });
    int ran = 0;
    for (const TickChunk& chunk : chunks) {
        if (!is_active(TickCell{ chunk.x * chunkSize_, 0, chunk.z * chunkSize_ })) continue;
        if (budget >= 0 && ran >= budget) break;
        const TickCell cell = random_cell(chunk.x, chunk.z, tick_);
        if (handlers_[index]) handlers_[index](cell);
        ++executed_[index];
        ++ran;
    }
}

void WorldScheduler::run_scheduled_phase() {
    const int index = static_cast<int>(Phase::ScheduledTick);
    if (scheduledPending_.empty()) return;
    const int budget = budgets_[index];
    std::vector<TickCell> due;
    due.reserve(scheduledPending_.size());
    for (const auto& entry : scheduledPending_) {
        // Overdue deadlines still run (no event is lost); they keep their
        // place in sorted order, so a backlog never reorders work.
        if (entry.second <= tick_) due.push_back(entry.first);
    }
    std::sort(due.begin(), due.end(), coord_less);
    int ran = 0;
    for (const TickCell& cell : due) {
        if (!is_active(cell)) continue;
        if (budget >= 0 && ran >= budget) break;
        scheduledPending_.erase(cell);
        if (handlers_[index]) handlers_[index](cell);
        ++executed_[index];
        ++ran;
    }
}

void WorldScheduler::run_neighbor_phase() {
    const int index = static_cast<int>(Phase::NeighborTick);
    if (neighborCenters_.empty() || !neighborHandler_) return;
    const int budget = budgets_[index];
    // Changed centers dispatch their neighbors in sorted (x, y, z) order and
    // each center dispatches its six orthogonal neighbors in the FIXED order
    // (-x, +x, -y, +y, -z, +z): deterministic propagation, never hash order.
    // A center is atomic: it dispatches all six neighbors or stays queued, so
    // a budget cut never loses (or half-drops) a center's propagation.
    std::vector<TickCell> centers(neighborCenters_.begin(), neighborCenters_.end());
    std::sort(centers.begin(), centers.end(), coord_less);
    // Fixed per-axis offset order.
    static const int axes[6] = { 0, 0, 1, 1, 2, 2 };
    static const int signs[6] = { -1, 1, -1, 1, -1, 1 };
    int ran = 0;
    for (const TickCell& center : centers) {
        if (!is_active(center)) continue;  // sleeping center wakes later
        // Atomic dispatch: all six must fit the remaining budget. When it does
        // not fit, the center stays queued (never partially dropped); a budget
        // unit here is one center (six neighbor dispatches).
        if (budget >= 0 && ran + 1 > budget) break;
        neighborCenters_.erase(center);
        for (int i = 0; i < 6; ++i) {
            TickCell neighbor = center;
            if (axes[i] == 0) neighbor.x += signs[i];
            else if (axes[i] == 1) neighbor.y += signs[i];
            else neighbor.z += signs[i];
            neighborHandler_(NeighborUpdate{ center, neighbor, axes[i], signs[i] });
            ++executed_[index];
        }
        ++ran;
    }
}

TickCell WorldScheduler::random_cell(int chunkX, int chunkZ, uint64_t tick) const {
    uint64_t h = seed_ ^ 0x9e3779b97f4a7c15ull;
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(chunkX)) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(chunkZ)) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
    h ^= tick + 0x9e3779b9u + (h << 6u) + (h >> 2u);
    // splitmix64 finalizer: bijective, so every (seed, chunk, tick) maps to a
    // well-distributed cell without any runtime state.
    h += 0x9e3779b97f4a7c15ull;
    h = (h ^ (h >> 30u)) * 0xbf58476d1ce4e5b9ull;
    h = (h ^ (h >> 27u)) * 0x94d049bb133111ebull;
    h ^= h >> 31u;
    const int x = static_cast<int>(h % static_cast<uint64_t>(chunkSize_));
    const int z = static_cast<int>((h >> 11u) % static_cast<uint64_t>(chunkSize_));
    const int y = static_cast<int>((h >> 22u) % static_cast<uint64_t>(randomHeight_));
    return TickCell{ chunkX * chunkSize_ + x, y, chunkZ * chunkSize_ + z };
}

bool WorldScheduler::is_active(const TickCell& cell) const {
    if (activeCenterX_ == -1) return true;  // no active region: everything ticks
    const int cx = floor_div(cell.x, chunkSize_);
    const int cz = floor_div(cell.z, chunkSize_);
    const int distance = std::max(std::abs(cx - activeCenterX_), std::abs(cz - activeCenterZ_));
    return activeRadius_ < 0 ? distance == 0 : distance <= activeRadius_;
}

int WorldScheduler::floor_div(int value, int divisor) {
    int q = value / divisor;
    if (value % divisor != 0 && ((value < 0) != (divisor < 0))) --q;
    return q;
}
