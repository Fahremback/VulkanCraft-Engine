#pragma once

// ---------------------------------------------------------------------------
// ScriptHotReload.hpp
//
// Hot reload for script files. Watches a set of script paths and detects
// changes by polling file mtime AND size (no OS file watcher needed, which
// keeps this portable and testable). When a change is detected the file is
// recompiled through an injectable compile callback and the resulting
// ScriptProgram is swapped into the ScriptVM atomically:
//
//   1. snapshot the VM's global variables
//   2. vm.load(newProgram)
//   3. restore the globals  → state survives the reload
//   4. notify listeners with on_reloaded-style callbacks
//
// The default compiler uses the real pipeline (ScriptGraphAsset::load +
// ScriptCompiler::compile). NOTE: ScriptGraphAsset::load is currently a
// placeholder in the engine, so inject a compiler with set_compiler() to
// load actual source files today.
// ---------------------------------------------------------------------------

#include "engine/scripting/ScriptRuntime.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine::Scripting {

class ScriptHotReload final {
public:
    /// Compiles a script file into a ScriptProgram.
    using CompileFn = std::function<ScriptCompileResult(const std::filesystem::path&)>;
    /// Called after every reload attempt. success=false carries a message.
    using ReloadListener = std::function<void(const std::filesystem::path&, bool success,
                                              const std::string& message)>;

    /// Default pipeline: ScriptGraphAsset::load + ScriptCompiler::compile.
    /// (The graph load step is stubbed in the engine today — see header.)
    static CompileFn default_compiler();

    ScriptHotReload() = default;

    void set_compiler(CompileFn compiler) { compiler_ = std::move(compiler); }
    void set_listener(ReloadListener listener) { listener_ = std::move(listener); }

    /// Start tracking a script file. Returns false if the file does not
    /// exist (nothing is tracked). Already-watched files are left untouched.
    bool watch(const std::filesystem::path& scriptFile);
    bool unwatch(const std::filesystem::path& scriptFile);
    bool is_watching(const std::filesystem::path& scriptFile) const;
    size_t watched_count() const { return entries_.size(); }
    std::vector<std::filesystem::path> watched_files() const;

    /// mtime+size comparison against the last recorded stamp.
    bool file_changed(const std::filesystem::path& scriptFile) const;
    /// Number of watched files whose stamp differs from the recorded one.
    size_t changed_count() const;

    /// Recompile `scriptFile`, swap the program into `vm` and preserve the
    /// VM's global variables. Returns true on success. The stamp is always
    /// refreshed so a broken file is not retried on every poll — it is
    /// retried on the next edit.
    bool reload(ScriptVM& vm, const std::filesystem::path& scriptFile);

    /// Check every watched file and reload the changed ones.
    /// Returns the number of successful reloads.
    size_t poll(ScriptVM& vm);

    // --- Global-state preservation -------------------------------------
    /// Snapshot the VM's global variables (name → value).
    static std::unordered_map<std::string, ScriptValue> snapshot_globals(const ScriptVM& vm);
    /// Re-apply a snapshot taken by snapshot_globals().
    static void restore_globals(ScriptVM& vm,
                                const std::unordered_map<std::string, ScriptValue>& globals);

private:
    struct FileStamp {
        std::filesystem::file_time_type mtime{};
        uintmax_t size{ 0 };
    };
    struct Entry {
        std::filesystem::path path;
        FileStamp stamp;
    };

    bool stamp_for(const std::filesystem::path& path, FileStamp& out) const;
    Entry* find_entry(const std::filesystem::path& path);
    const Entry* find_entry(const std::filesystem::path& path) const;

    std::vector<Entry> entries_;
    CompileFn compiler_{ default_compiler() };
    ReloadListener listener_;
};

} // namespace Engine::Scripting
