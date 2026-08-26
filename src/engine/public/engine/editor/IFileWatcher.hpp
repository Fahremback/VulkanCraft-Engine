#pragma once

// IFileWatcher (agente 2 §D, efsw "watcher + hot reload com debounce"):
// the NATIVE filesystem watcher that feeds IFileChangeDebounce. The debounce
// contract coalesces a burst of raw watcher events into one settled change
// per path; this contract owns the OS watcher (efsw) and delivers RAW events
// as FileChangeEvents, which the caller feeds into the debounce.
//   - PLATFORM: efsw is vendored at external/solutions/efsw (v1.6.3) and uses
//     the native backend per OS (ReadDirectoryChangesW on Windows, inotify on
//     Linux, FSEvents/kqueue on macOS) — no polling in the hot path.
//   - LIFECYCLE: start_watch() begins watching a directory (optionally
//     recursive); events accumulate in an internal queue; poll_events()
//     drains and returns them since the last poll; stop() stops and clears.
//   - THREADING: efsw delivers events on its own watch thread; the adapter
//     enqueues them under a mutex so poll_events() is safe from the caller's
//     thread. The queue is bounded (drops oldest beyond kMaxQueuedEvents) so
//     a slow consumer can never grow memory without bound.
//   - ERROR: start_watch() reports failure via errorOut (path missing,
//     backend unavailable, ...) and returns false — all-or-nothing, never
//     guessed.
//
// Self-contained (std only). The SDK adapter
// (src/engine/sdk/FileWatcher.cpp) is the ONLY TU with behavior.

#include "engine/editor/IFileChangeDebounce.hpp"

#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace editor {

// One directory being watched. `watchId` is the efsw WatchID (stable for the
// lifetime of the watch); `path` is the directory as passed to start_watch.
struct WatchHandle {
    long watchId{ -1 };
    std::string path;
};

// Native watcher over a directory tree. Events are FileChangeEvents (same
// type the debounce consumes), with `tick` set to the efsw event order
// (monotonic counter owned by the adapter; the debounce only needs relative
// order, so the counter is sufficient — no wall clock).
class IFileWatcher {
public:
    virtual ~IFileWatcher() = default;

    // Starts watching `directory` (recursively when recursive is true).
    // Returns false with a reason in errorOut on failure (all-or-nothing:
    // nothing is watched when false). Starting twice on the same path is a
    // no-op returning true with the existing handle.
    virtual bool start_watch(const std::string& directory, bool recursive,
                             std::string& errorOut) = 0;

    // Stops watching `directory` (all-or-nothing: unknown paths are ignored).
    virtual void stop_watch(const std::string& directory) = 0;

    // Stops all watches and clears the event queue. Safe to call twice.
    virtual void stop() = 0;

    // Drains and returns the raw events accumulated since the last call.
    // Order is the efsw delivery order (per-backend; cross-directory order is
    // not guaranteed — the debounce only coalesces per path, so this is fine).
    virtual std::vector<FileChangeEvent> poll_events() = 0;

    // Number of directories currently watched (diagnostics / tests).
    virtual std::size_t watch_count() const = 0;

    // Reads the accumulated watch list (diagnostics / tests).
    virtual std::vector<WatchHandle> watches() const = 0;
};

// Creates the native watcher (efsw backend). Never returns null.
std::unique_ptr<IFileWatcher> create_file_watcher();

}  // namespace editor
}  // namespace engine
