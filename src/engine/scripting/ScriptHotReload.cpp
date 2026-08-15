#include "engine/scripting/ScriptHotReload.hpp"

namespace Engine::Scripting {

ScriptHotReload::CompileFn ScriptHotReload::default_compiler() {
    return [](const std::filesystem::path& path) -> ScriptCompileResult {
        ScriptGraphAsset graph;
        if (!graph.load(path)) {
            ScriptCompileResult result;
            result.success = false;
            result.diagnostics.push_back(
                { UUID(), "Failed to load script graph from '" + path.string() + "'" });
            return result;
        }
        return ScriptCompiler::compile(graph);
    };
}

bool ScriptHotReload::watch(const std::filesystem::path& scriptFile) {
    if (find_entry(scriptFile) != nullptr) return true; // already tracked
    FileStamp stamp;
    if (!stamp_for(scriptFile, stamp)) return false;
    entries_.push_back({ scriptFile, stamp });
    return true;
}

bool ScriptHotReload::unwatch(const std::filesystem::path& scriptFile) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->path == scriptFile) {
            entries_.erase(it);
            return true;
        }
    }
    return false;
}

bool ScriptHotReload::is_watching(const std::filesystem::path& scriptFile) const {
    return find_entry(scriptFile) != nullptr;
}

std::vector<std::filesystem::path> ScriptHotReload::watched_files() const {
    std::vector<std::filesystem::path> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) out.push_back(entry.path);
    return out;
}

bool ScriptHotReload::stamp_for(const std::filesystem::path& path, FileStamp& out) const {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    out.mtime = std::filesystem::last_write_time(path, ec);
    out.size = std::filesystem::file_size(path, ec);
    return !ec;
}

bool ScriptHotReload::file_changed(const std::filesystem::path& scriptFile) const {
    const Entry* entry = find_entry(scriptFile);
    if (!entry) return false;
    FileStamp fresh;
    if (!stamp_for(scriptFile, fresh)) return false;
    return fresh.mtime != entry->stamp.mtime || fresh.size != entry->stamp.size;
}

size_t ScriptHotReload::changed_count() const {
    size_t count = 0;
    for (const auto& entry : entries_) {
        if (file_changed(entry.path)) ++count;
    }
    return count;
}

bool ScriptHotReload::reload(ScriptVM& vm, const std::filesystem::path& scriptFile) {
    Entry* entry = find_entry(scriptFile);
    if (!entry) return false;

    FileStamp fresh;
    if (!stamp_for(scriptFile, fresh)) return false;
    // Always refresh the stamp: a file that fails to compile must not be
    // retried on every poll (the next edit produces a new stamp).
    entry->stamp = fresh;

    ScriptCompileResult result = compiler_(scriptFile);
    if (!result) {
        const std::string message =
            result.diagnostics.empty() ? "compile failed"
                                       : result.diagnostics.front().message;
        if (listener_) listener_(scriptFile, false, message);
        return false;
    }

    // Atomic swap preserving global state: snapshot → load → restore.
    const std::unordered_map<std::string, ScriptValue> globals = snapshot_globals(vm);
    vm.load(std::move(result.program));
    restore_globals(vm, globals);

    if (listener_) listener_(scriptFile, true, "");
    return true;
}

size_t ScriptHotReload::poll(ScriptVM& vm) {
    size_t reloaded = 0;
    // Copy paths first so listener callbacks may safely unwatch/rewatch.
    const std::vector<std::filesystem::path> paths = watched_files();
    for (const auto& path : paths) {
        if (file_changed(path) && reload(vm, path)) ++reloaded;
    }
    return reloaded;
}

std::unordered_map<std::string, ScriptValue> ScriptHotReload::snapshot_globals(const ScriptVM& vm) {
    return vm.variables();
}

void ScriptHotReload::restore_globals(
    ScriptVM& vm, const std::unordered_map<std::string, ScriptValue>& globals) {
    for (const auto& [name, value] : globals) {
        vm.set_variable(name, value);
    }
}

ScriptHotReload::Entry* ScriptHotReload::find_entry(const std::filesystem::path& path) {
    for (auto& entry : entries_) {
        if (entry.path == path) return &entry;
    }
    return nullptr;
}

const ScriptHotReload::Entry* ScriptHotReload::find_entry(const std::filesystem::path& path) const {
    for (const auto& entry : entries_) {
        if (entry.path == path) return &entry;
    }
    return nullptr;
}

} // namespace Engine::Scripting
