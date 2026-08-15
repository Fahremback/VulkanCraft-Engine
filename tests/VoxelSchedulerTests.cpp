// VoxelSchedulerTests.cpp
//
// Deterministic world tick scheduler (META section 9): the engine owns the
// fixed-tick clock, the ORDER of tick work, budgets, dedup/cancel and active
// regions; the project owns the reactions. This TU proves the contracts:
//   1. Fixed tick clock: delta accumulates, a tick fires on every full step,
//      partial steps carry over, and a long hitch cannot spiral.
//   2. Phase order is fixed (block, random, fluid, scheduled, then the tick
//      callback) and deterministic across instances.
//   3. Budgets cap work per tick; overflow carries in the same sorted order.
//   4. Dedup: scheduling the same cell/chunk twice is one unit of work.
//   5. Cancel: cancel_cell and cancel_chunk remove pending work.
//   6. Active regions sleep far work and wake it when the center approaches;
//      sleeping work never consumes the budget.
//   7. Random ticks derive from (seed, chunk, tick): same seed => same cell,
//      different seed/tick => different cell, always inside the chunk.
//   8. Scheduled ticks run on their absolute deadline (and overdue ones are
//      never lost).
#include "WorldScheduler.hpp"

#include <iostream>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond  \
                      << "\n";                                                 \
        }                                                                      \
    } while (0)

void test_fixed_tick_clock() {
    WorldScheduler s;
    CHECK(s.current_tick() == 0);
    CHECK(s.advance(0.05f, 0.1f) == 0);   // not a full step yet
    CHECK(s.current_tick() == 0);
    CHECK(s.advance(0.05f, 0.1f) == 1);   // 0.05 + 0.05 = one step
    CHECK(s.current_tick() == 1);
    CHECK(s.advance(0.25f, 0.1f) == 2);   // two steps, 0.05 carried
    CHECK(s.current_tick() == 3);
    CHECK(s.advance(0.05f, 0.1f) == 1);   // carried 0.05 + 0.05 = one step
    CHECK(s.current_tick() == 4);
    // A long hitch executes the backlog but is capped (never spirals).
    CHECK(s.advance(1000.0f, 0.1f) == 128);
    CHECK(s.current_tick() == 4 + 128);
}

void test_phase_order() {
    std::vector<int> order;
    auto run = [&](WorldScheduler& s) {
        order.clear();
        s.set_handler(WorldScheduler::Phase::BlockTick,
                      [&](const TickCell&) { order.push_back(0); });
        s.set_handler(WorldScheduler::Phase::RandomTick,
                      [&](const TickCell&) { order.push_back(1); });
        s.set_handler(WorldScheduler::Phase::FluidTick,
                      [&](const TickCell&) { order.push_back(2); });
        s.set_handler(WorldScheduler::Phase::ScheduledTick,
                      [&](const TickCell&) { order.push_back(3); });
        s.set_tick_callback([&](uint64_t) { order.push_back(4); });
        s.schedule_block_tick(TickCell{ 1, 2, 3 });
        s.schedule_random_tick(0, 0);
        s.schedule_fluid_tick(TickCell{ 4, 5, 6 });
        s.schedule_scheduled_tick(TickCell{ 7, 8, 9 }, 0);
        s.advance(0.1f, 0.1f);
    };
    WorldScheduler a;
    run(a);
    CHECK(order.size() == 5);
    for (int i = 0; i < 5; ++i) CHECK(order[static_cast<std::size_t>(i)] == i);
    // Identical schedule + clock on a fresh instance => identical order.
    std::vector<int> first = order;
    WorldScheduler b;
    run(b);
    CHECK(order == first);
}

void test_block_priority_and_budget() {
    WorldScheduler s;
    s.set_budget(WorldScheduler::Phase::BlockTick, 2);
    std::vector<TickCell> ran;
    s.set_handler(WorldScheduler::Phase::BlockTick,
                  [&](const TickCell& c) { ran.push_back(c); });
    // Inserted unsorted, with mixed priorities.
    s.schedule_block_tick(TickCell{ 0, 0, 2 }, 1);
    s.schedule_block_tick(TickCell{ 0, 0, 0 }, 10);
    s.schedule_block_tick(TickCell{ 0, 0, 1 }, 0);
    s.schedule_block_tick(TickCell{ 0, 0, 4 }, 10);
    s.schedule_block_tick(TickCell{ 0, 0, 3 }, 0);

    s.advance(0.1f, 0.1f);
    // Priority desc first: {0,0,0} and {0,0,4}; ties in (x,y,z).
    CHECK(ran.size() == 2);
    CHECK((ran[0] == TickCell{ 0, 0, 0 }));
    CHECK((ran[1] == TickCell{ 0, 0, 4 }));
    CHECK(s.pending_count() == 3);

    s.advance(0.1f, 0.1f);
    // Next budget window: priority-1 {0,0,2}, then priority-0 {0,0,1}.
    CHECK(ran.size() == 4);
    CHECK((ran[2] == TickCell{ 0, 0, 2 }));
    CHECK((ran[3] == TickCell{ 0, 0, 1 }));

    s.advance(0.1f, 0.1f);
    CHECK(ran.size() == 5);
    CHECK((ran[4] == TickCell{ 0, 0, 3 }));
    CHECK(s.pending_count() == 0);
    CHECK(s.executed_count(WorldScheduler::Phase::BlockTick) == 5);
}

void test_dedup() {
    WorldScheduler s;
    int blockRuns = 0;
    int fluidRuns = 0;
    int randomRuns = 0;
    s.set_handler(WorldScheduler::Phase::BlockTick,
                  [&](const TickCell&) { ++blockRuns; });
    s.set_handler(WorldScheduler::Phase::FluidTick,
                  [&](const TickCell&) { ++fluidRuns; });
    s.set_handler(WorldScheduler::Phase::RandomTick,
                  [&](const TickCell&) { ++randomRuns; });
    s.schedule_block_tick(TickCell{ 1, 1, 1 });
    s.schedule_block_tick(TickCell{ 1, 1, 1 });   // duplicate
    s.schedule_fluid_tick(TickCell{ 2, 2, 2 });
    s.schedule_fluid_tick(TickCell{ 2, 2, 2 });   // duplicate
    s.schedule_random_tick(3, 4);
    s.schedule_random_tick(3, 4);                 // duplicate chunk
    CHECK(s.pending_count() == 3);                // 3 units, not 6
    s.advance(0.1f, 0.1f);
    CHECK(blockRuns == 1);
    CHECK(fluidRuns == 1);
    CHECK(randomRuns == 1);
    // Random ticks are persistent per scheduled chunk: one deterministic cell
    // per chunk per tick (the project cancels / moves the active region to
    // stop them). Block/fluid/scheduled work is consumed and gone.
    CHECK(s.pending_count() == 1);
    s.advance(0.1f, 0.1f);
    CHECK(randomRuns == 2);
}

void test_cancel() {
    WorldScheduler s;
    int runs = 0;
    s.set_handler(WorldScheduler::Phase::BlockTick, [&](const TickCell&) { ++runs; });
    s.set_handler(WorldScheduler::Phase::FluidTick, [&](const TickCell&) { ++runs; });
    s.set_handler(WorldScheduler::Phase::ScheduledTick, [&](const TickCell&) { ++runs; });

    s.schedule_block_tick(TickCell{ 1, 1, 1 });
    s.cancel_cell(TickCell{ 1, 1, 1 });
    s.advance(0.1f, 0.1f);
    CHECK(runs == 0);

    // Everything inside chunk (0,0) dies with cancel_chunk (including the
    // random chunk key and scheduled work).
    s.schedule_block_tick(TickCell{ 1, 1, 1 });
    s.schedule_fluid_tick(TickCell{ 1, 1, 2 });
    s.schedule_scheduled_tick(TickCell{ 1, 1, 3 }, 0);
    s.schedule_random_tick(0, 0);
    CHECK(s.pending_count() == 4);
    s.cancel_chunk(0, 0);
    CHECK(s.pending_count() == 0);
    s.advance(0.1f, 0.1f);
    CHECK(runs == 0);

    // A chunk outside the canceled one is untouched.
    int blockRuns = 0;
    s.set_handler(WorldScheduler::Phase::BlockTick, [&](const TickCell&) { ++blockRuns; });
    s.schedule_block_tick(TickCell{ 40, 5, 40 });  // chunk (2,2)
    s.cancel_chunk(0, 0);
    s.advance(0.1f, 0.1f);
    CHECK(blockRuns == 1);
}

void test_active_region_sleep_wake() {
    WorldScheduler s;
    std::vector<TickCell> ran;
    s.set_handler(WorldScheduler::Phase::FluidTick,
                  [&](const TickCell& c) { ran.push_back(c); });
    s.schedule_fluid_tick(TickCell{ 100, 5, 100 });  // chunk (6,6)
    s.schedule_fluid_tick(TickCell{ 1, 5, 1 });      // chunk (0,0)
    s.set_active_center(0, 0);
    s.set_active_radius(0);
    s.advance(0.1f, 0.1f);
    CHECK(ran.size() == 1);
    CHECK((ran[0] == TickCell{ 1, 5, 1 }));
    CHECK(s.pending_count() == 1);  // far cell sleeps, stays queued

    // The player approaches: the far cell wakes and executes.
    s.set_active_center(6, 6);
    s.advance(0.1f, 0.1f);
    CHECK(ran.size() == 2);
    CHECK((ran[1] == TickCell{ 100, 5, 100 }));
    CHECK(s.pending_count() == 0);

    // Sleeping work never consumes the budget.
    WorldScheduler b;
    int bRuns = 0;
    b.set_handler(WorldScheduler::Phase::FluidTick, [&](const TickCell&) { ++bRuns; });
    b.set_budget(WorldScheduler::Phase::FluidTick, 1);
    b.schedule_fluid_tick(TickCell{ 100, 5, 100 });  // sleeping
    b.schedule_fluid_tick(TickCell{ 1, 5, 1 });      // active
    b.set_active_center(0, 0);
    b.set_active_radius(0);
    b.advance(0.1f, 0.1f);
    CHECK(bRuns == 1);  // the active one took the only budget slot
    CHECK(b.pending_count() == 1);
}

void test_random_tick_seed_determinism() {
    auto cell_for = [](uint64_t seed) {
        WorldScheduler s(seed);
        std::vector<TickCell> ran;
        s.set_handler(WorldScheduler::Phase::RandomTick,
                      [&](const TickCell& c) { ran.push_back(c); });
        s.schedule_random_tick(0, 0);
        s.advance(0.1f, 0.1f);
        return ran.empty() ? TickCell{ -1, -1, -1 } : ran[0];
    };
    const TickCell a = cell_for(42);
    const TickCell b = cell_for(42);
    CHECK(a == b);                    // same seed => same cell
    const TickCell c = cell_for(43);
    CHECK(!(a == c));                 // different seed => different cell
    CHECK(a.x >= 0 && a.x < 16);      // inside chunk (0,0)
    CHECK(a.z >= 0 && a.z < 16);
    CHECK(a.y >= 0 && a.y < 128);

    // The cell advances with the tick: same chunk, next tick, new cell.
    WorldScheduler s(42);
    std::vector<TickCell> ran;
    s.set_handler(WorldScheduler::Phase::RandomTick,
                  [&](const TickCell& c) { ran.push_back(c); });
    s.schedule_random_tick(0, 0);
    s.advance(0.1f, 0.1f);
    const TickCell first = ran.back();
    s.advance(0.1f, 0.1f);
    CHECK(ran.size() == 2);
    CHECK(!(first == ran[1]));

    // Negative chunk coordinates are supported and stay inside the chunk.
    WorldScheduler neg(7);
    std::vector<TickCell> negRan;
    neg.set_handler(WorldScheduler::Phase::RandomTick,
                    [&](const TickCell& cell) { negRan.push_back(cell); });
    neg.schedule_random_tick(-1, -1);
    neg.advance(0.1f, 0.1f);
    CHECK(negRan.size() == 1);
    CHECK(negRan[0].x >= -16 && negRan[0].x < 0);
    CHECK(negRan[0].z >= -16 && negRan[0].z < 0);
}

void test_scheduled_deadline() {
    WorldScheduler s;
    std::vector<TickCell> ran;
    s.set_handler(WorldScheduler::Phase::ScheduledTick,
                  [&](const TickCell& c) { ran.push_back(c); });
    s.schedule_scheduled_tick(TickCell{ 5, 0, 0 }, 3);
    for (int i = 0; i < 3; ++i) {
        s.advance(0.1f, 0.1f);
        CHECK(ran.empty());
    }
    s.advance(0.1f, 0.1f);  // tick 3: deadline due
    CHECK(ran.size() == 1);
    CHECK((ran[0] == TickCell{ 5, 0, 0 }));
    CHECK(s.pending_count() == 0);

    // Overdue work is never lost: it runs at the next tick in sorted order.
    WorldScheduler late;
    std::vector<TickCell> lateRan;
    late.set_handler(WorldScheduler::Phase::ScheduledTick,
                     [&](const TickCell& c) { lateRan.push_back(c); });
    late.schedule_scheduled_tick(TickCell{ 9, 0, 0 }, 0);   // deadline already past
    late.schedule_scheduled_tick(TickCell{ 2, 0, 0 }, 0);
    late.schedule_scheduled_tick(TickCell{ 5, 0, 0 }, 100);  // far future stays
    late.advance(0.1f, 0.1f);
    CHECK(lateRan.size() == 2);
    CHECK((lateRan[0] == TickCell{ 2, 0, 0 }));   // sorted (x,y,z)
    CHECK((lateRan[1] == TickCell{ 9, 0, 0 }));
    CHECK(late.pending_count() == 1);
    late.advance(0.1f, 0.1f);
    CHECK(lateRan.size() == 2);  // future deadline still pending
}

}  // namespace

int main() {
    test_fixed_tick_clock();
    test_phase_order();
    test_block_priority_and_budget();
    test_dedup();
    test_cancel();
    test_active_region_sleep_wake();
    test_random_tick_seed_determinism();
    test_scheduled_deadline();

    if (g_failures == 0) {
        std::cout << "voxel_scheduler_tests: all checks passed\n";
        return 0;
    }
    std::cerr << "voxel_scheduler_tests: " << g_failures << " check(s) failed\n";
    return 1;
}
