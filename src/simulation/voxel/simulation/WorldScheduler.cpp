#include "WorldScheduler.hpp"

#include <algorithm>
#include <cstdlib>
#include <tuple>
#include <utility>

namespace {
// Deterministic coordinate ordering shared by every phase: (x, y, z) ascending.
bool coord_less(const TickCell& a, const TickCell& b) {
    return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
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

void WorldScheduler::cancel_cell(TickCell cell) {
    blockPending_.erase(cell);
    fluidPending_.erase(cell);
    scheduledPending_.erase(cell);
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
}

void WorldScheduler::set_handler(Phase phase, Handler handler) {
    handlers_[static_cast<int>(phase)] = std::move(handler);
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
           fluidPending_.size() + scheduledPending_.size();
}

void WorldScheduler::run_tick() {
    run_phase(Phase::BlockTick);
    run_random_phase();
    run_phase(Phase::FluidTick);
    run_scheduled_phase();
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
