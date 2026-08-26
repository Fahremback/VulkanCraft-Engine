#pragma once

// IFileChangeDebounce (agente 2 §D, efsw "watcher + hot reload com debounce"):
// the PURE debounce/coalescing core for asset hot reload. A native watcher
// (efsw) emits a BURST of filesystem events for a single editor save (create,
// then several writes). This contract coalesces that burst into ONE settled
// change per path, after a quiet window with no further events.
//   - DETERMINISTIC: the clock is INJECTED (monotonic `tick`, caller-owned),
//     so the same sequence of events + ticks yields identical settled output,
//     bit-exact. No wall clock, no threads.
//   - COALESCE (last-kind-wins): events on the same path before settling
//     collapse to the LAST kind (create+modify -> modify = "file exists, reload";
//     delete+create -> create = "reload"; create+delete -> delete = "unload").
//     This is the correct net semantics for a reloader.
//   - QUIET WINDOW: a pending change settles when (nowTick - lastTick) >=
//     quiet_ticks. Each new event restarts the window for that path.
//   - ANTI-STARVATION: max_hold_ticks > 0 caps how long a continuously-written
//     path may stay pending (settles early once (nowTick - firstTick) >=
//     max_hold_ticks), so a busy file can never block reload forever.
//   - FLUSH: settles everything immediately (shutdown). Deterministic order.
//
// Self-contained (std only). The SDK adapter
// (src/engine/sdk/FileChangeDebounce.cpp) is the ONLY TU with behavior.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace editor {

enum class FileChangeKind : std::uint8_t { Modified, Created, Deleted, Renamed };

// One raw watcher event. `tick` is the injected monotonic time; callers feed
// events with non-decreasing ticks.
struct FileChangeEvent {
    std::string path;
    FileChangeKind kind{ FileChangeKind::Modified };
    std::uint64_t tick{ 0 };

    bool operator==(const FileChangeEvent& other) const {
        return path == other.path && kind == other.kind && tick == other.tick;
    }
    bool operator!=(const FileChangeEvent& other) const {
        return !(*this == other);
    }
};

// One settled (debounced) change, ready for a reloader.
struct FileChangeSettled {
    std::string path;
    FileChangeKind kind{ FileChangeKind::Modified };
    std::uint64_t settled_tick{ 0 };  // last event tick + quiet window

    bool operator==(const FileChangeSettled& other) const {
        return path == other.path && kind == other.kind &&
               settled_tick == other.settled_tick;
    }
    bool operator!=(const FileChangeSettled& other) const {
        return !(*this == other);
    }
};

struct DebounceSpec {
    int version{ 1 };
    std::uint64_t quiet_ticks{ 5 };     // quiet window (>= 1)
    std::uint64_t max_hold_ticks{ 0 };  // 0 = unlimited; else >= 1 (anti-starvation)

    bool operator==(const DebounceSpec& other) const {
        return version == other.version && quiet_ticks == other.quiet_ticks &&
               max_hold_ticks == other.max_hold_ticks;
    }
    bool operator!=(const DebounceSpec& other) const {
        return !(*this == other);
    }

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    std::string to_json() const;
};

class IFileChangeDebounce {
public:
    virtual ~IFileChangeDebounce() = default;

    // Feeds a raw event. Events must have non-decreasing `tick` (a later
    // event on the same path restarts its quiet window). Always succeeds.
    virtual void record(const FileChangeEvent& event) = 0;

    // Advances the injected clock and returns changes whose quiet window (or
    // max-hold cap) has elapsed, in deterministic order (by settle tick, then
    // path). A change is returned exactly once.
    virtual std::vector<FileChangeSettled> advance(std::uint64_t nowTick) = 0;

    // Settles every pending change immediately (shutdown), deterministic
    // order (settle tick, then path). Leaves the model empty.
    virtual std::vector<FileChangeSettled> flush() = 0;

    // Number of distinct paths currently pending settlement.
    virtual std::size_t pending_count() const = 0;

    // Clears all pending state without settling.
    virtual void reset() = 0;

    virtual const DebounceSpec& spec() const = 0;
};

// Validates the spec and creates the debouncer (rejected -> nullptr +
// errorOut).
std::unique_ptr<IFileChangeDebounce> create_file_change_debounce(
    const DebounceSpec& spec, std::string& errorOut);

}  // namespace editor
}  // namespace engine
