// FileChangeDebounce.cpp — the ONLY TU with the hot-reload debounce behavior
// (agente 2 §D). PURE and deterministic: coalesces a burst of filesystem
// events into one settled change per path using an INJECTED monotonic clock.
// Last-kind-wins per path; quiet window restarts on each new event; an
// optional max-hold cap prevents starvation. No wall clock/threads/OS calls.

#include "engine/editor/IFileChangeDebounce.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <map>
#include <sstream>

namespace engine {
namespace editor {

namespace {

struct PendingChange {
    std::uint64_t first_tick{ 0 };
    std::uint64_t last_tick{ 0 };
    FileChangeKind kind{ FileChangeKind::Modified };
};

const char* kind_name(FileChangeKind kind) {
    switch (kind) {
        case FileChangeKind::Modified: return "modified";
        case FileChangeKind::Created: return "created";
        case FileChangeKind::Deleted: return "deleted";
        case FileChangeKind::Renamed: return "renamed";
    }
    return "modified";
}

bool kind_from_name(const std::string& name, FileChangeKind& out) {
    if (name == "modified") { out = FileChangeKind::Modified; return true; }
    if (name == "created") { out = FileChangeKind::Created; return true; }
    if (name == "deleted") { out = FileChangeKind::Deleted; return true; }
    if (name == "renamed") { out = FileChangeKind::Renamed; return true; }
    return false;
}

}  // namespace

bool DebounceSpec::validate(std::string& errorOut) const {
    errorOut.clear();
    if (version != 1) {
        errorOut = "unsupported debounce spec version";
        return false;
    }
    if (quiet_ticks < 1) {
        errorOut = "debounce quiet_ticks must be >= 1";
        return false;
    }
    return true;
}

std::string DebounceSpec::to_json() const {
    std::ostringstream out;
    out << "{\"version\":" << version << ",\"quiet_ticks\":" << quiet_ticks
        << ",\"max_hold_ticks\":" << max_hold_ticks << "}";
    return out.str();
}

bool DebounceSpec::load_from_json(const std::string& jsonText,
                                  std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(jsonText, doc, errorOut)) return false;
    if (!doc.is_object()) {
        errorOut = "debounce spec document must be an object";
        return false;
    }
    const int version = static_cast<int>(sdk::json_number(doc, "version", 1));
    if (version != 1) {
        errorOut = "unsupported debounce spec version";
        return false;
    }
    DebounceSpec candidate;
    candidate.version = version;
    candidate.quiet_ticks = static_cast<std::uint64_t>(
        sdk::json_number(doc, "quiet_ticks", 5.0));
    candidate.max_hold_ticks = static_cast<std::uint64_t>(
        sdk::json_number(doc, "max_hold_ticks", 0.0));
    if (!candidate.validate(errorOut)) return false;
    *this = std::move(candidate);
    return true;
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

namespace {

class FileChangeDebounceRuntime final : public IFileChangeDebounce {
public:
    explicit FileChangeDebounceRuntime(const DebounceSpec& spec) : spec_(spec) {}

    void record(const FileChangeEvent& event) override {
        auto it = pending_.find(event.path);
        if (it == pending_.end()) {
            PendingChange pc;
            pc.first_tick = event.tick;
            pc.last_tick = event.tick;
            pc.kind = event.kind;
            pending_.emplace(event.path, pc);
            return;
        }
        // Existing: last-kind-wins, window restarts on later tick.
        PendingChange& pending = it->second;
        if (pending.last_tick < event.tick) {
            pending.last_tick = event.tick;
        }
        pending.kind = event.kind;
    }

    std::vector<FileChangeSettled> advance(std::uint64_t nowTick) override {
        std::vector<std::pair<std::uint64_t, std::string>> order;
        for (const auto& entry : pending_) {
            const PendingChange& pending = entry.second;
            bool settle = (nowTick - pending.last_tick) >= spec_.quiet_ticks;
            if (!settle && spec_.max_hold_ticks > 0) {
                settle = (nowTick - pending.first_tick) >= spec_.max_hold_ticks;
            }
            if (settle) {
                order.emplace_back(pending.last_tick, entry.first);
            }
        }
        std::sort(order.begin(), order.end());
        std::vector<FileChangeSettled> out;
        out.reserve(order.size());
        for (const auto& entry : order) {
            const PendingChange& pending = pending_[entry.second];
            FileChangeSettled settled;
            settled.path = entry.second;
            settled.kind = pending.kind;
            settled.settled_tick = pending.last_tick + spec_.quiet_ticks;
            out.push_back(std::move(settled));
            pending_.erase(entry.second);
        }
        return out;
    }

    std::vector<FileChangeSettled> flush() override {
        std::vector<std::pair<std::uint64_t, std::string>> order;
        for (const auto& entry : pending_) {
            order.emplace_back(entry.second.last_tick, entry.first);
        }
        std::sort(order.begin(), order.end());
        std::vector<FileChangeSettled> out;
        out.reserve(order.size());
        for (const auto& entry : order) {
            const PendingChange& pending = pending_[entry.second];
            FileChangeSettled settled;
            settled.path = entry.second;
            settled.kind = pending.kind;
            settled.settled_tick = pending.last_tick + spec_.quiet_ticks;
            out.push_back(std::move(settled));
        }
        pending_.clear();
        return out;
    }

    std::size_t pending_count() const override { return pending_.size(); }

    void reset() override { pending_.clear(); }

    const DebounceSpec& spec() const override { return spec_; }

private:
    DebounceSpec spec_;
    std::map<std::string, PendingChange> pending_;
};

}  // namespace

std::unique_ptr<IFileChangeDebounce> create_file_change_debounce(
    const DebounceSpec& spec, std::string& errorOut) {
    errorOut.clear();
    if (!spec.validate(errorOut)) return nullptr;
    return std::make_unique<FileChangeDebounceRuntime>(spec);
}

}  // namespace editor
}  // namespace engine
