// FileChangeDebounceTests — headless coverage for the public hot-reload
// debounce contract (engine/editor/IFileChangeDebounce.hpp, adapter
// FileChangeDebounce.cpp): coalescing a burst of filesystem events into one
// settled change per path (injected monotonic clock -> deterministic),
// last-kind-wins, quiet window, max-hold anti-starvation, flush, and spec
// round-trip. Standalone main() with CHECK.

#include "engine/editor/IFileChangeDebounce.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace engine::editor;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FileChangeDebounceTests failure at line " << __LINE__ << ": " #condition "\n"; \
    return false; } } while (false)

namespace {

bool run_all() {
    std::string err;

    // ---- Spec validate + JSON round-trip --------------------------------
    {
        DebounceSpec spec;
        spec.quiet_ticks = 5;
        spec.max_hold_ticks = 20;
        CHECK(spec.validate(err));
        const std::string json = spec.to_json();
        DebounceSpec back;
        CHECK(back.load_from_json(json, err));
        CHECK(back.quiet_ticks == 5);
        CHECK(back.max_hold_ticks == 20);
        CHECK(back.to_json() == json);

        DebounceSpec zero;
        zero.quiet_ticks = 0;
        CHECK(!zero.validate(err));
        CHECK(!err.empty());

        DebounceSpec untouched = spec;
        CHECK(!untouched.load_from_json("{bad", err));
        CHECK(untouched.quiet_ticks == 5);
        CHECK(!untouched.load_from_json("{\"version\":99}", err));
        CHECK(untouched.quiet_ticks == 5);
    }

    // ---- Coalescing a burst into one settled change ----------------------
    {
        DebounceSpec spec;
        spec.quiet_ticks = 5;
        auto db = create_file_change_debounce(spec, err);
        CHECK(db != nullptr);

        // A save produces: create@1, write@2, write@3 (same path).
        db->record({"res/foo.png", FileChangeKind::Created, 1});
        db->record({"res/foo.png", FileChangeKind::Modified, 2});
        db->record({"res/foo.png", FileChangeKind::Modified, 3});
        CHECK(db->pending_count() == 1);

        // Not settled yet (now - last = 4 < 5).
        CHECK(db->advance(7).empty());
        CHECK(db->pending_count() == 1);

        // Settled at now >= last + quiet (3 + 5 = 8).
        const auto settled = db->advance(8);
        CHECK(settled.size() == 1);
        CHECK(settled[0].path == "res/foo.png");
        CHECK(settled[0].kind == FileChangeKind::Modified);  // last-kind-wins
        CHECK(settled[0].settled_tick == 8);
        CHECK(db->pending_count() == 0);
    }

    // ---- Window restarts on each new event ------------------------------
    {
        DebounceSpec spec;
        spec.quiet_ticks = 5;
        auto db = create_file_change_debounce(spec, err);
        CHECK(db != nullptr);

        db->record({"a.txt", FileChangeKind::Modified, 0});
        CHECK(db->advance(4).empty());   // 4 < 5
        db->record({"a.txt", FileChangeKind::Modified, 4});  // restart window
        CHECK(db->advance(8).empty());   // 8 - 4 = 4 < 5
        const auto settled = db->advance(9);  // 9 - 4 = 5
        CHECK(settled.size() == 1);
        CHECK(settled[0].settled_tick == 9);
    }

    // ---- Multiple paths settle in deterministic order --------------------
    {
        DebounceSpec spec;
        spec.quiet_ticks = 3;
        auto db = create_file_change_debounce(spec, err);
        CHECK(db != nullptr);

        db->record({"b.txt", FileChangeKind::Modified, 5});
        db->record({"a.txt", FileChangeKind::Modified, 2});
        const auto settled = db->advance(10);  // both settled
        CHECK(settled.size() == 2);
        CHECK(settled[0].path == "a.txt");  // earlier last_tick first
        CHECK(settled[1].path == "b.txt");
    }

    // ---- max_hold anti-starvation ----------------------------------------
    {
        DebounceSpec spec;
        spec.quiet_ticks = 5;
        spec.max_hold_ticks = 10;
        auto db = create_file_change_debounce(spec, err);
        CHECK(db != nullptr);

        db->record({"busy.log", FileChangeKind::Modified, 0});
        // Continuously written: never quiet for 5 ticks, but held >= 10.
        db->record({"busy.log", FileChangeKind::Modified, 4});
        db->record({"busy.log", FileChangeKind::Modified, 8});
        CHECK(db->advance(9).empty());  // held 9 < 10, quiet 1 < 5
        const auto settled = db->advance(10);  // held 10 >= 10 -> settle
        CHECK(settled.size() == 1);
        CHECK(settled[0].path == "busy.log");
    }

    // ---- last-kind-wins net semantics (create+delete -> delete) ----------
    {
        DebounceSpec spec;
        spec.quiet_ticks = 1;
        auto db = create_file_change_debounce(spec, err);
        CHECK(db != nullptr);

        db->record({"x.bin", FileChangeKind::Created, 0});
        db->record({"x.bin", FileChangeKind::Deleted, 1});
        const auto settled = db->advance(2);
        CHECK(settled.size() == 1);
        CHECK(settled[0].kind == FileChangeKind::Deleted);  // net unload
    }

    // ---- flush settles everything, deterministic -------------------------
    {
        DebounceSpec spec;
        spec.quiet_ticks = 5;
        auto db = create_file_change_debounce(spec, err);
        CHECK(db != nullptr);

        db->record({"z.txt", FileChangeKind::Modified, 1});
        db->record({"a.txt", FileChangeKind::Created, 0});
        const auto settled = db->flush();
        CHECK(settled.size() == 2);
        CHECK(settled[0].path == "a.txt");
        CHECK(settled[1].path == "z.txt");
        CHECK(db->pending_count() == 0);
    }

    // ---- reset ------------------------------------------------------------
    {
        DebounceSpec spec;
        spec.quiet_ticks = 5;
        auto db = create_file_change_debounce(spec, err);
        CHECK(db != nullptr);
        db->record({"a.txt", FileChangeKind::Modified, 0});
        CHECK(db->pending_count() == 1);
        db->reset();
        CHECK(db->pending_count() == 0);
        CHECK(db->advance(100).empty());
    }

    // ---- Determinism cross-instance --------------------------------------
    {
        DebounceSpec spec;
        spec.quiet_ticks = 4;
        for (int i = 0; i < 2; ++i) {
            auto db = create_file_change_debounce(spec, err);
            CHECK(db != nullptr);
            db->record({"m.png", FileChangeKind::Created, 1});
            db->record({"m.png", FileChangeKind::Modified, 2});
            db->record({"n.png", FileChangeKind::Modified, 3});
            const auto settled = db->advance(20);
            CHECK(settled.size() == 2);
            if (i == 0) {
                CHECK(settled[0].path == "m.png");
                CHECK(settled[0].kind == FileChangeKind::Modified);
                CHECK(settled[1].path == "n.png");
            } else {
                CHECK(settled[0].path == "m.png");
                CHECK(settled[0].kind == FileChangeKind::Modified);
                CHECK(settled[1].path == "n.png");
            }
        }
    }

    std::cout << "FileChangeDebounceTests: all checks passed\n";
    return true;
}

}  // namespace

int main() {
    if (!run_all()) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
