#include "PluginRuntime.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <sstream>

namespace Engine::Plugins {

PluginManifest::Validation PluginManifest::validate() const {
    Validation result;
    if (name.empty()) result.errors.emplace_back("plugin name must not be empty");
    if (name.find_first_of("<>:\"/\\|?*") != std::string::npos)
        result.errors.emplace_back("plugin name contains invalid path characters");
    if (apiVersion == 0)
        result.errors.emplace_back("apiVersion must be >= 1");
    if (version.empty())
        result.errors.emplace_back("version must not be empty");
    for (const std::string& dep : dependencies) {
        if (dep == name) result.errors.emplace_back("plugin cannot depend on itself");
    }
    // Duplicate dependency names.
    std::vector<std::string> sorted = dependencies;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
        result.errors.emplace_back("duplicate dependencies");
    return result;
}

bool PluginManifest::save_to_file(const std::filesystem::path& path) const {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << "VCPLUGIN 1\n";
    out << "name " << name << "\n";
    out << "version " << version << "\n";
    out << "apiVersion " << apiVersion << "\n";
    out << "editorOnly " << (editorOnly ? 1 : 0) << "\n";
    out << "dependencies " << dependencies.size() << "\n";
    for (const std::string& dep : dependencies) out << "  dep " << dep << "\n";
    out << "optionalDependencies " << optionalDependencies.size() << "\n";
    for (const std::string& dep : optionalDependencies) out << "  dep " << dep << "\n";
    out << "description " << description << "\n";
    return out.good();
}

bool PluginManifest::load_from_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string magic;
    int formatVersion{};
    if (!(in >> magic >> formatVersion) || magic != "VCPLUGIN" || formatVersion != 1) return false;
    std::string key;
    while (in >> key) {
        if (key == "name") in >> name;
        else if (key == "version") in >> version;
        else if (key == "apiVersion") {
            unsigned v{};
            in >> v;
            apiVersion = v;
        } else if (key == "editorOnly") {
            int v{};
            in >> v;
            editorOnly = v != 0;
        } else if (key == "dependencies") {
            size_t count{};
            in >> count;
            dependencies.clear();
            for (size_t i = 0; i < count; ++i) {
                std::string tag, dep;
                in >> tag >> dep;
                if (tag == "dep") dependencies.push_back(dep);
            }
        } else if (key == "optionalDependencies") {
            size_t count{};
            in >> count;
            optionalDependencies.clear();
            for (size_t i = 0; i < count; ++i) {
                std::string tag, dep;
                in >> tag >> dep;
                if (tag == "dep") optionalDependencies.push_back(dep);
            }
        } else if (key == "description") {
            std::getline(in, description);
            if (!description.empty() && description.front() == ' ') description.erase(description.begin());
        }
    }
    return true;
}

bool PluginRuntime::register_factory(std::string name, Factory factory) {
    if (name.empty() || !factory) return false;
    auto [it, inserted] = records_.emplace(std::move(name), Record{});
    if (!inserted) return false;
    it->second.factory = std::move(factory);
    return true;
}

PluginRuntime::Record* PluginRuntime::find(const std::string& name) {
    const auto it = records_.find(name);
    return it == records_.end() ? nullptr : &it->second;
}

const PluginRuntime::Record* PluginRuntime::find(const std::string& name) const {
    const auto it = records_.find(name);
    return it == records_.end() ? nullptr : &it->second;
}

bool PluginRuntime::api_compatible(std::uint32_t pluginApi, std::uint32_t engineApi) noexcept {
    // Plugins targeting the current major API or older are accepted; plugins
    // requiring a future API are rejected.
    return pluginApi <= engineApi && pluginApi > 0;
}

bool PluginRuntime::compute_load_order(std::vector<std::string>& order,
                                       std::string* error) const {
    order.clear();
    std::unordered_map<std::string, int> state;  // 0=unvisited, 1=visiting, 2=done
    std::function<bool(const std::string&)> visit = [&](const std::string& name) -> bool {
        const auto it = records_.find(name);
        if (it == records_.end()) return true;  // unknown dep treated as satisfied (optional-ish)
        const int s = state[name];
        if (s == 2) return true;
        if (s == 1) {
            if (error) *error = "dependency cycle involving " + name;
            return false;
        }
        state[name] = 1;
        for (const std::string& dep : it->second.plugin.manifest.dependencies) {
            if (records_.contains(dep) && !visit(dep)) return false;
        }
        state[name] = 2;
        order.push_back(name);
        return true;
    };
    for (const auto& [name, _] : records_) {
        if (!visit(name)) return false;
    }
    return true;
}

bool PluginRuntime::begin_load(Record& record, std::string& error) {
    // Already loaded or already on the current load chain (cycle).
    if (record.plugin.loaded) return true;
    if (std::find(loading_.begin(), loading_.end(), record.plugin.manifest.name) != loading_.end()) {
        error = "dependency cycle involving '" + record.plugin.manifest.name + "'";
        return false;
    }
    loading_.push_back(record.plugin.manifest.name);
    return true;
}

void PluginRuntime::end_load(Record& record) noexcept {
    loading_.erase(std::remove(loading_.begin(), loading_.end(), record.plugin.manifest.name),
                   loading_.end());
}

bool PluginRuntime::load_record(Record& record, std::string& error) {
    if (record.plugin.loaded) return true;
    if (!begin_load(record, error)) return false;
    struct Guard {
        PluginRuntime& self;
        Record& record;
        ~Guard() { self.end_load(record); }
    } guard{*this, record};

    const PluginManifest& manifest = record.plugin.manifest;
    const auto validation = manifest.validate();
    if (!validation.valid()) {
        error = "manifest invalid: " + validation.errors.front();
        return false;
    }
    if (!api_compatible(manifest.apiVersion, 1)) {
        error = "plugin '" + manifest.name + "' requires apiVersion " +
                std::to_string(manifest.apiVersion) + " (engine supports 1)";
        return false;
    }
    if (!record.factory) {
        error = "no runtime factory registered for plugin '" + manifest.name + "'";
        return false;
    }
    // Load dependencies first (already ordered via compute_load_order).
    for (const std::string& dep : manifest.dependencies) {
        Record* depRecord = find(dep);
        if (!depRecord) {
            error = "missing dependency '" + dep + "' for plugin '" + manifest.name + "'";
            return false;
        }
        if (!depRecord->plugin.loaded) {
            std::string depError;
            if (!load_record(*depRecord, depError)) {
                error = "dependency '" + dep + "' failed: " + depError;
                return false;
            }
        }
    }
    record.plugin.runtime = record.factory(manifest);
    if (!record.plugin.runtime) {
        error = "factory returned null for '" + manifest.name + "'";
        return false;
    }
    if (!record.plugin.runtime->on_load(error)) return false;
    record.plugin.loaded = true;
    record.plugin.loadError.clear();
    return true;
}

bool PluginRuntime::load_manifest(const PluginManifest& manifest, std::string* error) {
    std::string localError;
    if (!manifest.validate().valid()) {
        if (error) *error = "manifest invalid";
        return false;
    }
    auto it = records_.find(manifest.name);
    if (it == records_.end()) {
        Record record;
        record.plugin.manifest = manifest;
        it = records_.emplace(manifest.name, std::move(record)).first;
    } else {
        it->second.plugin.manifest = manifest;
    }
    const bool ok = load_record(it->second, localError);
    if (!ok && error) *error = localError;
    return ok;
}

bool PluginRuntime::load_manifest_file(const std::filesystem::path& path, std::string* error) {
    PluginManifest manifest;
    if (!manifest.load_from_file(path)) {
        if (error) *error = "cannot parse manifest: " + path.string();
        return false;
    }
    return load_manifest(manifest, error);
}

bool PluginRuntime::unload(const std::string& name, std::string* error) {
    Record* record = find(name);
    if (!record) {
        if (error) *error = "unknown plugin '" + name + "'";
        return false;
    }
    // Guard against dependency cycles between loaded plugins: never unload the
    // same plugin twice on one call chain.
    static thread_local std::vector<std::string> unloading;
    if (std::find(unloading.begin(), unloading.end(), name) != unloading.end()) return true;
    unloading.push_back(name);
    struct Guard {
        const std::string& name;
        ~Guard() {
            unloading.erase(std::remove(unloading.begin(), unloading.end(), name), unloading.end());
        }
    } guard{name};
    // Unload dependents first (reverse dependency order).
    for (auto& [otherName, other] : records_) {
        if (otherName == name || !other.plugin.loaded) continue;
        for (const std::string& dep : other.plugin.manifest.dependencies) {
            if (dep == name) {
                std::string ignore;
                if (!unload(otherName, &ignore)) {
                    if (error) *error = "cannot unload dependent '" + otherName + "'";
                    return false;
                }
                break;
            }
        }
    }
    if (record->plugin.loaded) {
        record->plugin.runtime->on_unload();
        record->plugin.loaded = false;
        record->plugin.runtime.reset();
    }
    return true;
}

bool PluginRuntime::reload(const std::string& name, std::string* error) {
    Record* record = find(name);
    if (!record) {
        if (error) *error = "unknown plugin '" + name + "'";
        return false;
    }
    const PluginManifest manifest = record->plugin.manifest;
    if (!unload(name, error)) return false;
    record->plugin.manifest = manifest;
    std::string localError;
    if (!load_record(*record, localError)) {
        if (error) *error = "reload failed: " + localError;
        return false;
    }
    return true;
}

void PluginRuntime::update_all(double deltaSeconds) {
    std::vector<std::string> order;
    if (!compute_load_order(order, nullptr)) return;
    for (const std::string& name : order) {
        Record* record = find(name);
        if (record && record->plugin.loaded && record->plugin.runtime) {
            record->plugin.runtime->update(deltaSeconds);
        }
    }
}

std::vector<std::string> PluginRuntime::loaded_names() const {
    std::vector<std::string> names;
    for (const auto& [name, record] : records_) {
        if (record.plugin.loaded) names.push_back(name);
    }
    return names;
}

bool PluginRuntime::is_loaded(const std::string& name) const {
    const Record* record = find(name);
    return record && record->plugin.loaded;
}

const LoadedPlugin* PluginRuntime::plugin(const std::string& name) const {
    const Record* record = find(name);
    return record ? &record->plugin : nullptr;
}

} // namespace Engine::Plugins
