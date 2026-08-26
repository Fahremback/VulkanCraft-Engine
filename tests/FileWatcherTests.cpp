// Gate for engine::editor::IFileWatcher (agente 2 §D, efsw native watcher).
// Contract-level checks (error handling, watch list, no-op rewatch) are
// deterministic; the end-to-end check exercises the REAL efsw backend with a
// temp directory (create file -> event must arrive within a bounded poll
// window). The debounce contract (IFileChangeDebounce) is fed the raw event
// and must settle it after a quiet window.

#include "engine/editor/IFileChangeDebounce.hpp"
#include "engine/editor/IFileWatcher.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d — %s\n", __FILE__, __LINE__, msg); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

std::filesystem::path make_temp_dir(const char* tag) {
    const auto root = std::filesystem::temp_directory_path();
    const auto dir = root / (std::string("vc_filewatcher_") + tag);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

// Polls the watcher until a raw event for `needle` appears (or timeout),
// ACCUMULATING every drained event into `out` so the caller can feed the
// debounce afterwards.
bool wait_for_event(engine::editor::IFileWatcher& w, const std::string& needle,
                    std::chrono::milliseconds timeout,
                    std::vector<engine::editor::FileChangeEvent>& out) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool found = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto events = w.poll_events();
        for (const auto& e : events) {
            out.push_back(e);
            if (e.path.find(needle) != std::string::npos) found = true;
        }
        if (found) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return found;
}

void test_contract_validation() {
    auto w = engine::editor::create_file_watcher();
    CHECK(w != nullptr, "create_file_watcher never returns null");
    CHECK(w->watch_count() == 0, "no watches initially");

    std::string err;
    CHECK(!w->start_watch("", false, err), "empty path refused");
    CHECK(!err.empty(), "refusal carries a reason");
    CHECK(w->watch_count() == 0, "failed watch adds nothing");
}

void test_watch_list_and_noop_rewatch() {
    const auto dir = make_temp_dir("list");
    auto w = engine::editor::create_file_watcher();
    std::string err;

    CHECK(w->start_watch(dir.string(), true, err), "start_watch ok");
    CHECK(err.empty(), "no error on success");
    CHECK(w->watch_count() == 1, "one watch registered");
    const auto list = w->watches();
    CHECK(list.size() == 1, "watches() lists the directory");
    CHECK(list[0].path == dir.string(), "watch path matches");

    // Rewatching the same path is a no-op (still one watch).
    CHECK(w->start_watch(dir.string(), true, err), "rewatch ok");
    CHECK(w->watch_count() == 1, "rewatch does not duplicate");

    w->stop_watch(dir.string());
    CHECK(w->watch_count() == 0, "stop_watch removes it");

    // stop() is idempotent.
    w->stop();
    w->stop();
    CHECK(w->watch_count() == 0, "stop() idempotent");
}

void test_end_to_end_create_event() {
    const auto dir = make_temp_dir("e2e");
    auto w = engine::editor::create_file_watcher();
    std::string err;
    CHECK(w->start_watch(dir.string(), true, err), "watch temp dir");

    // Create a file -> efsw must deliver a Created/Modified event.
    const auto file = dir / "new_asset.txt";
    {
        std::ofstream out(file);
        out << "hello\n";
    }

    std::vector<engine::editor::FileChangeEvent> collected;
    CHECK(wait_for_event(*w, "new_asset.txt", std::chrono::milliseconds(4000), collected),
          "create event arrives within poll window");

    // Feed the raw events into the debounce and advance the injected clock
    // past the quiet window — a real watcher + the deterministic debounce
    // core compose into the hot-reload pipeline.
    engine::editor::DebounceSpec spec;
    std::string specErr;
    spec.quiet_ticks = 2;
    auto debounce = engine::editor::create_file_change_debounce(spec, specErr);
    CHECK(debounce != nullptr, "debounce created");
    std::uint64_t tick = 1000;
    for (const auto& e : collected) {
        debounce->record(engine::editor::FileChangeEvent{ e.path, e.kind, tick++ });
    }
    const auto settled = debounce->advance(tick + 100);
    CHECK(!settled.empty(), "debounce settles the watched event");

    w->stop();
}

void test_end_to_end_delete_event() {
    const auto dir = make_temp_dir("e2e_del");
    const auto file = dir / "doomed.txt";
    {
        std::ofstream out(file);
        out << "bye\n";
    }
    auto w = engine::editor::create_file_watcher();
    std::string err;
    CHECK(w->start_watch(dir.string(), true, err), "watch temp dir");

    std::filesystem::remove(file);
    std::vector<engine::editor::FileChangeEvent> collected;
    CHECK(wait_for_event(*w, "doomed.txt", std::chrono::milliseconds(4000), collected),
          "delete event arrives within poll window");
    CHECK(!collected.empty(), "delete test collected events");
    w->stop();
}

}  // namespace

int main() {
    test_contract_validation();
    test_watch_list_and_noop_rewatch();
    test_end_to_end_create_event();
    test_end_to_end_delete_event();

    if (g_failures == 0) {
        std::printf("file_watcher_tests: ALL PASSED\n");
        return 0;
    }
    std::printf("file_watcher_tests: %d FAILURE(S)\n", g_failures);
    return 1;
}
