// IFileWatcher adapter backed by the vendored efsw watcher (external/solutions/efsw).
// efsw delivers events on its own thread; we enqueue them under a mutex and
// drain on the caller's thread via poll_events(). The `tick` field is a
// monotonic counter incremented per enqueued event (relative order only —
// the debounce contract needs no wall clock).

#include "engine/editor/IFileWatcher.hpp"

#include <efsw/efsw.hpp>

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <unordered_map>

namespace engine {
namespace editor {
namespace {

constexpr std::size_t kMaxQueuedEvents = 4096;

FileChangeKind map_action(efsw::Action action) {
    switch (action) {
        case efsw::Actions::Add: return FileChangeKind::Created;
        case efsw::Actions::Delete: return FileChangeKind::Deleted;
        case efsw::Actions::Modified: return FileChangeKind::Modified;
        case efsw::Actions::Moved: return FileChangeKind::Renamed;
    }
    return FileChangeKind::Modified;
}

class Listener final : public efsw::FileWatchListener {
public:
    void handleFileAction(efsw::WatchID /*watchid*/, const std::string& dir,
                          const std::string& filename, efsw::Action action,
                          const std::string& /*oldFilename*/) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kMaxQueuedEvents) {
            // Bounded queue: drop the oldest so a slow consumer can never
            // grow memory without bound.
            queue_.erase(queue_.begin());
        }
        std::string path = dir;
        if (!path.empty() && path.back() != '/' && path.back() != '\\') {
            path += '/';
        }
        path += filename;
        queue_.push_back(FileChangeEvent{ std::move(path), map_action(action), ++counter_ });
    }

    std::vector<FileChangeEvent> drain() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<FileChangeEvent> out;
        out.swap(queue_);
        return out;
    }

private:
    std::mutex mutex_;
    std::vector<FileChangeEvent> queue_;
    std::uint64_t counter_{ 0 };
};

class FileWatcherImpl final : public IFileWatcher {
public:
    FileWatcherImpl() = default;

    ~FileWatcherImpl() override { stop(); }

    bool start_watch(const std::string& directory, bool recursive,
                     std::string& errorOut) override {
        if (directory.empty()) {
            errorOut = "watch directory must not be empty";
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (watches_.count(directory) != 0) {
            return true;  // already watching — no-op
        }
        if (!watcher_) {
            watcher_ = std::make_unique<efsw::FileWatcher>();
            watcher_->watch();  // starts the efsw watch thread
        }
        const efsw::WatchID id = watcher_->addWatch(directory, &listener_, recursive);
        if (id < 0) {
            errorOut = "efsw addWatch failed for " + directory;
            return false;
        }
        watches_.emplace(directory, WatchHandle{ static_cast<long>(id), directory });
        return true;
    }

    void stop_watch(const std::string& directory) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = watches_.find(directory);
        if (it == watches_.end()) return;
        if (watcher_) watcher_->removeWatch(static_cast<efsw::WatchID>(it->second.watchId));
        watches_.erase(it);
    }

    void stop() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (watcher_) {
            for (const auto& kv : watches_) {
                watcher_->removeWatch(kv.first);
            }
            watcher_.reset();
        }
        watches_.clear();
        listener_.drain();  // drop queued events
    }

    std::vector<FileChangeEvent> poll_events() override {
        return listener_.drain();
    }

    std::size_t watch_count() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return watches_.size();
    }

    std::vector<WatchHandle> watches() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<WatchHandle> out;
        out.reserve(watches_.size());
        for (const auto& kv : watches_) out.push_back(kv.second);
        return out;
    }

private:
    // mutable because watch_count()/watches() are const and take the lock.
    mutable std::mutex mutex_;
    std::unique_ptr<efsw::FileWatcher> watcher_;
    std::unordered_map<std::string, WatchHandle> watches_;
    Listener listener_;
};

}  // namespace

std::unique_ptr<IFileWatcher> create_file_watcher() {
    return std::make_unique<FileWatcherImpl>();
}

}  // namespace editor
}  // namespace engine
