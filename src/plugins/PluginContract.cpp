#include "PluginContract.hpp"

#include "engine/packaging/IPackageManager.hpp"
#include "engine/plugins/IPluginIsolation.hpp"
#include "engine/plugins/IPluginLoader.hpp"
#include "engine/plugins/IPluginManifestCodec.hpp"
#include "engine/plugins/IPluginPermissions.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace {

class UnifiedManifestManager final : public engine::plugins::IPluginManifestManager {
public:
    explicit UnifiedManifestManager(engine::plugins::IPluginManifestCodec& codec) : codec_(codec) {}

    bool register_manifest(const engine::plugins::PluginManifest& manifest, std::string& error) override {
        if (manifests_.count(manifest.name)) { error = "duplicate_plugin"; return false; }
        std::string encoded;
        engine::plugins::PluginManifest decoded;
        if (!codec_.encode(manifest, encoded, error) || !codec_.decode(encoded, decoded, error)) return false;
        manifests_[manifest.name] = decoded;
        engine::plugins::PluginRuntimeInfo info;
        info.manifest = decoded;
        info.state = engine::plugins::PluginState::Unloaded;
        runtime_[manifest.name] = std::move(info);
        error.clear();
        return true;
    }

    bool unregister(const std::string& name, std::string& error) override {
        if (!manifests_.erase(name)) { error = "plugin_not_found"; return false; }
        runtime_.erase(name);
        error.clear();
        return true;
    }

    const engine::plugins::PluginManifest* get(const std::string& name) const override {
        const auto it = manifests_.find(name);
        return it == manifests_.end() ? nullptr : &it->second;
    }

    std::vector<engine::plugins::PluginManifest> list() const override {
        std::vector<engine::plugins::PluginManifest> out;
        for (const auto& [name, manifest] : manifests_) { (void)name; out.push_back(manifest); }
        return out;
    }

    std::vector<std::string> resolve_dependencies(std::string& error) const override {
        std::map<std::string, int> state;
        std::vector<std::string> order;
        std::function<bool(const std::string&)> visit = [&](const std::string& name) {
            const auto it = manifests_.find(name);
            if (it == manifests_.end()) { error = "plugin_not_found:" + name; return false; }
            if (state[name] == 2) return true;
            if (state[name] == 1) { error = "dependency_cycle:" + name; return false; }
            state[name] = 1;
            for (const auto& dependency : it->second.dependencies) {
                const auto dep = manifests_.find(dependency.name);
                if (dep == manifests_.end()) {
                    if (dependency.required) { error = "missing_dependency:" + dependency.name; return false; }
                    continue;
                }
                if (!dependency.constraint.satisfies(dep->second.version)) {
                    error = "dependency_version:" + dependency.name;
                    return false;
                }
                if (!visit(dependency.name)) return false;
            }
            state[name] = 2;
            order.push_back(name);
            return true;
        };
        for (const auto& [name, manifest] : manifests_) {
            (void)manifest;
            if (!visit(name)) return {};
        }
        error.clear();
        return order;
    }

    bool can_load(const std::string& name, std::string& error) const override {
        if (!get(name)) { error = "plugin_not_found"; return false; }
        auto order = resolve_dependencies(error);
        return !order.empty() || manifests_.size() == 1;
    }

    const engine::plugins::PluginRuntimeInfo* get_runtime_info(const std::string& name) const override {
        const auto it = runtime_.find(name);
        return it == runtime_.end() ? nullptr : &it->second;
    }

    bool set_state(const std::string& name, engine::plugins::PluginState state,
                   std::string& error) override {
        const auto it = runtime_.find(name);
        if (it == runtime_.end()) { error = "plugin_not_found"; return false; }
        it->second.state = state;
        error.clear();
        return true;
    }

private:
    engine::plugins::IPluginManifestCodec& codec_;
    std::map<std::string, engine::plugins::PluginManifest> manifests_;
    std::map<std::string, engine::plugins::PluginRuntimeInfo> runtime_;
};

class LegacyIsolationAdapter final : public engine::plugins::IPluginIsolation {
public:
    LegacyIsolationAdapter(engine::plugins::IPluginIsolationManager& runtime, std::string name,
                           std::uint64_t timeout, std::uint64_t memory)
        : runtime_(runtime), name_(std::move(name)), timeout_(timeout), memory_(memory) {}

    const std::string& plugin_name() const override { return name_; }
    std::uint32_t failure_count() const override { return failures_; }
    const engine::plugins::PluginFailure* last_failure() const override {
        return last_ ? &*last_ : nullptr;
    }
    bool is_healthy() const override { return runtime_.healthy(name_) && !critical_; }
    void record_failure(const engine::plugins::PluginFailure& failure) override {
        last_ = failure;
        ++failures_;
        critical_ = critical_ || failure.severity >= engine::plugins::FailureSeverity::Critical;
        for (const auto& handler : handlers_) if (handler) handler(failure);
    }
    bool try_recovery(std::string& error) override {
        std::string ignored;
        (void)runtime_.unload(name_, ignored);
        if (!runtime_.register_plugin(name_, timeout_, memory_, error)) return false;
        critical_ = false;
        failures_ = 0;
        last_.reset();
        return true;
    }
    bool force_unload(bool, std::string& error) override { return runtime_.unload(name_, error); }
    void reset_failure_count() override { failures_ = 0; last_.reset(); critical_ = false; }
    void on_failure(engine::plugins::FailureHandler handler) override { handlers_.push_back(std::move(handler)); }

private:
    engine::plugins::IPluginIsolationManager& runtime_;
    std::string name_;
    std::uint64_t timeout_{0};
    std::uint64_t memory_{0};
    std::uint32_t failures_{0};
    bool critical_{false};
    std::optional<engine::plugins::PluginFailure> last_;
    std::vector<engine::plugins::FailureHandler> handlers_;
};

struct UnifiedPluginServices {
    UnifiedPluginServices() {
        loader = engine::plugins::create_plugin_loader();
        permissions = engine::plugins::create_plugin_permission_policy();
        isolationRuntime = engine::plugins::create_plugin_isolation_runtime();
        manifestCodec = engine::plugins::create_plugin_manifest_codec();
        std::string error;
        packages = engine::packaging::create_package_manager("engine.plugins", error);
        if (manifestCodec) manifests = std::make_unique<UnifiedManifestManager>(*manifestCodec);
    }

    engine::plugins::PluginManifest manifest_for(const Engine::Plugins::EnginePlugin& plugin) const {
        engine::plugins::PluginManifest manifest;
        manifest.name = plugin.get_name();
        manifest.display_name = plugin.get_name();
        manifest.description = "VulkanCraft runtime plugin";
        manifest.author = "VulkanCraft";
        manifest.version = engine::plugins::PluginVersion::parse(plugin.get_version());
        manifest.abi = engine::plugins::PluginAbi::Cpp;
        manifest.permissions = {
            engine::plugins::Permissions::kAssetRead,
            engine::plugins::Permissions::kWorldRead};
        return manifest;
    }

    bool begin_load(const Engine::Plugins::EnginePlugin& plugin, bool editor, std::string& error) {
        if (!loader || !permissions || !isolationRuntime || !manifestCodec || !manifests || !packages) {
            error = "plugin_runtime_services_unavailable";
            return false;
        }
        auto manifest = manifest_for(plugin);
        if (editor) manifest.permissions.push_back(engine::plugins::Permissions::kEditorAccess);

        if (!manifests->get(manifest.name) && !manifests->register_manifest(manifest, error)) return false;
        (void)manifests->set_state(manifest.name, engine::plugins::PluginState::Loading, error);

        engine::plugins::PluginPermissions grants;
        for (const auto& permission : manifest.permissions) {
            grants.required.insert(permission);
            (void)permissions->grant(permission, manifest.name);
            if (permissions->is_granted(permission, manifest.name)) grants.granted.insert(permission);
        }
        engine::plugins::PluginLoadOptions options;
        options.permissions = grants;
        options.timeout_ms = 1000;
        options.memory_limit_bytes = 256u * 1024u * 1024u;
        options.allow_dynamic_code = false;

        bool known = false;
        for (const auto& runtime : loader->runtime()) if (runtime.manifest.name == manifest.name) known = true;
        if (!known && !loader->register_plugin(manifest, options, error)) return false;
        if (!loader->load(manifest.name, error)) return false;
        const auto* sandbox = loader->sandbox(manifest.name);
        if (!sandbox || !sandbox->check_all(manifest.permissions)) {
            error = "plugin_sandbox_permission_mismatch";
            return false;
        }
        if (!isolation.count(manifest.name)) {
            if (!isolationRuntime->register_plugin(manifest.name, options.timeout_ms,
                                                   options.memory_limit_bytes, error)) return false;
            isolation[manifest.name] = std::make_unique<LegacyIsolationAdapter>(
                *isolationRuntime, manifest.name, options.timeout_ms, options.memory_limit_bytes);
        }
        if (!isolationRuntime->begin_call(manifest.name, error)) return false;

        engine::plugins::PluginRegistration registration;
        registration.kind = engine::plugins::PluginRegistrationKind::Type;
        registration.id = "plugin." + manifest.name;
        registration.name = manifest.display_name;
        registration.description = manifest.description;
        registration.pluginName = manifest.name;
        registration.version = manifest.version.to_string();
        if (!loader->registry().is_registered(registration.id) &&
            !loader->registry().register_item(std::move(registration), &error)) return false;

        std::string encoded;
        if (!manifestCodec->encode(manifest, encoded, error)) return false;
        engine::packaging::PackageManifest package;
        package.name = "plugin." + manifest.name;
        package.version = manifest.version.to_string();
        package.content_hash = encoded;
        if (!packages->register_manifest(package, error)) {
            const auto states = packages->states();
            const bool already = std::any_of(states.begin(), states.end(), [&](const auto& state) {
                return state.name == package.name && state.version == package.version &&
                       state.content_hash == package.content_hash;
            });
            if (!already) return false;
            error.clear();
        }
        return true;
    }

    bool end_load(const std::string& name, std::string& error) {
        if (!isolationRuntime->end_call(name, 0, 0, error)) return false;
        return manifests->set_state(name, engine::plugins::PluginState::Loaded, error);
    }

    void fail_load(const std::string& name, const std::string& message) {
        const auto found = isolation.find(name);
        if (found != isolation.end()) {
            engine::plugins::PluginFailure failure;
            failure.plugin_name = name;
            failure.severity = engine::plugins::FailureSeverity::Critical;
            failure.error_type = "load";
            failure.error_message = message;
            found->second->record_failure(failure);
        }
        std::string ignored;
        if (manifests) (void)manifests->set_state(name, engine::plugins::PluginState::Error, ignored);
    }

    void unload(const std::string& name) {
        std::string ignored;
        if (loader) (void)loader->unload(name, ignored);
        if (isolationRuntime) (void)isolationRuntime->unload(name, ignored);
        isolation.erase(name);
        if (manifests) (void)manifests->set_state(name, engine::plugins::PluginState::Unloaded, ignored);
        if (packages) (void)packages->uninstall("plugin." + name, ignored);
    }

    std::unique_ptr<engine::plugins::IPluginLoader> loader;
    std::unique_ptr<engine::plugins::IPluginPermissionPolicy> permissions;
    std::unique_ptr<engine::plugins::IPluginIsolationManager> isolationRuntime;
    std::unique_ptr<engine::plugins::IPluginManifestCodec> manifestCodec;
    std::unique_ptr<UnifiedManifestManager> manifests;
    std::unique_ptr<engine::packaging::IPackageManager> packages;
    std::unordered_map<std::string, std::unique_ptr<LegacyIsolationAdapter>> isolation;
};

UnifiedPluginServices& unified_services() {
    static UnifiedPluginServices services;
    return services;
}

} // namespace

namespace Engine::Plugins {
void EnginePlugin::bind(PluginContext context) {
    if (loaded_) throw std::logic_error("Cannot rebind a loaded plugin");
    context_ = std::make_unique<PluginContext>(std::move(context));
}
PluginContext& EnginePlugin::context() {
    if (!context_) throw std::logic_error("Plugin context was not bound");
    return *context_;
}
void EnginePlugin::on_load() {
    if (loaded_) return;
    auto& ctx = context();
    auto& services = unified_services();
    std::string serviceError;
    if (!services.begin_load(*this, ctx.editor != nullptr, serviceError)) {
        services.fail_load(get_name(), serviceError);
        throw std::runtime_error("Plugin '" + get_name() + "' runtime gate failed: " + serviceError);
    }
    try {
        register_types(ctx.types);
        register_assets(ctx.assets);
        if (ctx.editor) register_editor_tools(*ctx.editor);
        startup();
        if (!services.end_load(get_name(), serviceError)) {
            throw std::runtime_error(serviceError);
        }
        loaded_ = true;
    } catch (const std::exception& exception) {
        services.fail_load(get_name(), exception.what());
        services.unload(get_name());
        throw;
    } catch (...) {
        services.fail_load(get_name(), "unknown_plugin_exception");
        services.unload(get_name());
        throw;
    }
}
void EnginePlugin::on_unload() {
    if (!loaded_) return;
    shutdown();
    if (context_ && context_->unregisterEditorOwner)
        context_->unregisterEditorOwner(get_name());
    unified_services().unload(get_name());
    loaded_ = false;
}
void register_plugin_type(TypeRegistry& registry, std::string name) {
    ClassMetaData metadata;
    metadata.name = std::move(name);
    registry.register_class(metadata);
}
engine::plugins::IPluginLoader* product_plugin_loader() noexcept {
    return unified_services().loader.get();
}
engine::plugins::IPluginPermissionPolicy* product_plugin_permissions() noexcept {
    return unified_services().permissions.get();
}
engine::plugins::IPluginTypeRegistry* product_plugin_type_registry() noexcept {
    auto* loader = unified_services().loader.get();
    return loader ? &loader->registry() : nullptr;
}
} // namespace Engine::Plugins

namespace {

// Product composition root for the plugin contract.  vc_editor_extensions is
// linked into the shipped game and editor, so this object participates in
// their real process boot/shutdown instead of leaving EnginePlugin reachable
// only from tests or SDK helpers.  Registering through PluginRegistry invokes
// EnginePlugin::on_load(), which in turn drives the canonical loader,
// permissions, sandbox, isolation, type registry, manifest and package
// services above.  Shutdown invokes the symmetric on_unload() path.
class ProductRuntimePlugin final : public Engine::Plugins::EnginePlugin {
public:
    std::string get_name() const override { return "RuntimeCore"; }
    std::string get_version() const override { return "1.0.0"; }

protected:
    void register_types(Engine::TypeRegistry& registry) override {
        Engine::Plugins::register_plugin_type(registry, "RuntimePluginService");
    }
    void register_assets(Engine::AssetRegistry&) override {}
};

class ProductPluginBootstrap final {
public:
    ProductPluginBootstrap() noexcept {
        try {
            plugin_ = std::make_shared<ProductRuntimePlugin>();
            Engine::Plugins::PluginContext context{
                Engine::TypeRegistry::get(), runtimeAssets_, nullptr, {}};
            plugin_->bind(std::move(context));
            auto& registry = Engine::PluginRegistry::get();
            registry.register_plugin(plugin_);
            // Exercise the production reload transition through the same
            // registry used by runtime enable/disable: on_unload tears down
            // loader/isolation/package state and on_load reconstructs it.
            // RuntimeCore is intentionally registration-only, so this boot
            // self-cycle is deterministic and does not disturb game state.
            if (!registry.set_plugin_enabled(plugin_->get_name(), false) ||
                !registry.set_plugin_enabled(plugin_->get_name(), true)) {
                throw std::runtime_error("product plugin reload lifecycle failed");
            }
            loaded_ = plugin_->is_loaded();
        } catch (const std::exception& exception) {
            error_ = exception.what();
            loaded_ = false;
        } catch (...) {
            error_ = "unknown_product_plugin_boot_error";
            loaded_ = false;
        }
        if (!loaded_) {
            std::fprintf(stderr, "[Plugin] RuntimeCore bootstrap failed: %s\n", error_.c_str());
        }
    }

    ~ProductPluginBootstrap() {
        // EnginePlugin::on_unload() is idempotent and performs the canonical
        // loader/isolation/package teardown.  Calling it directly avoids
        // static-destruction ordering assumptions about PluginRegistry.
        if (plugin_ && plugin_->is_loaded()) plugin_->on_unload();
    }

private:
    Engine::AssetRegistry runtimeAssets_;
    std::shared_ptr<ProductRuntimePlugin> plugin_;
    bool loaded_{false};
    std::string error_;
};

ProductPluginBootstrap g_productPluginBootstrap;

} // namespace
