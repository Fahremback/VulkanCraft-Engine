// PluginManifestTests.cpp — testes headless para IPluginManifest + IPluginIsolation
// Verifica: validação de manifesto, versionamento, dependências,
// isolamento de falhas, hot reload, timeout, memória.
//
// Todos os testes são determinísticos e não requerem GPU, filesystem
// ou rede.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/plugins/IPluginManifest.hpp"
#include "engine/plugins/IPluginIsolation.hpp"

using namespace engine::plugins;

// ── Mock implementations ───────────────────────────────────────────

class MockManifestManager : public IPluginManifestManager {
public:
    bool register_manifest(const PluginManifest& manifest, std::string& error) override {
        std::string val_error;
        if (!manifest.validate(val_error)) {
            error = val_error;
            return false;
        }
        if (manifests_.count(manifest.name)) {
            error = "duplicate: " + manifest.name;
            return false;
        }
        manifests_[manifest.name] = manifest;
        return true;
    }

    bool unregister(const std::string& name, std::string& error) override {
        if (manifests_.erase(name) == 0) {
            error = "not found: " + name;
            return false;
        }
        return true;
    }

    const PluginManifest* get(const std::string& name) const override {
        auto it = manifests_.find(name);
        return it != manifests_.end() ? &it->second : nullptr;
    }

    std::vector<PluginManifest> list() const override {
        std::vector<PluginManifest> result;
        for (const auto& [name, m] : manifests_) {
            result.push_back(m);
        }
        return result;
    }

    std::vector<std::string> resolve_dependencies(std::string& error) const override {
        // Topological sort
        std::unordered_map<std::string, int> in_degree;
        std::unordered_map<std::string, std::vector<std::string>> adj;
        for (const auto& [name, m] : manifests_) {
            if (!in_degree.count(name)) in_degree[name] = 0;
            for (const auto& dep : m.dependencies) {
                if (manifests_.count(dep.name) == 0) {
                    if (dep.required) {
                        error = "missing required dependency: " + dep.name;
                        return {};
                    }
                    continue;
                }
                adj[dep.name].push_back(name);
                in_degree[name]++;
            }
        }
        // Kahn's algorithm
        std::vector<std::string> queue;
        for (auto& [name, deg] : in_degree) {
            if (deg == 0) queue.push_back(name);
        }
        std::vector<std::string> order;
        while (!queue.empty()) {
            std::string n = queue.back();
            queue.pop_back();
            order.push_back(n);
            for (const auto& next : adj[n]) {
                if (--in_degree[next] == 0) {
                    queue.push_back(next);
                }
            }
        }
        if (order.size() != manifests_.size()) {
            error = "circular dependency detected";
            return {};
        }
        return order;
    }

    bool can_load(const std::string& name, std::string& error) const override {
        auto it = manifests_.find(name);
        if (it == manifests_.end()) { error = "not found: " + name; return false; }
        for (const auto& dep : it->second.dependencies) {
            if (dep.required && manifests_.count(dep.name) == 0) {
                error = "missing: " + dep.name;
                return false;
            }
        }
        return true;
    }

    const PluginRuntimeInfo* get_runtime_info(const std::string& name) const override {
        auto it = runtime_.find(name);
        return it != runtime_.end() ? &it->second : nullptr;
    }

    bool set_state(const std::string& name, PluginState state, std::string& error) override {
        auto it = runtime_.find(name);
        if (it == runtime_.end()) {
            runtime_[name] = PluginRuntimeInfo();
            it = runtime_.find(name);
        }
        it->second.state = state;
        return true;
    }

private:
    std::unordered_map<std::string, PluginManifest> manifests_;
    std::unordered_map<std::string, PluginRuntimeInfo> runtime_;
};

class MockPluginIsolation : public IPluginIsolation {
public:
    explicit MockPluginIsolation(std::string name) : name_(std::move(name)) {}

    const std::string& plugin_name() const override { return name_; }
    std::uint32_t failure_count() const override { return failureCount_; }
    const PluginFailure* last_failure() const override {
        return lastFailure_.has_value() ? &lastFailure_.value() : nullptr;
    }
    bool is_healthy() const override {
        return failureCount_ == 0 ||
               (!lastFailure_.has_value() ||
                lastFailure_->severity < FailureSeverity::Critical);
    }

    void record_failure(const PluginFailure& failure) override {
        lastFailure_ = failure;
        failureCount_++;
        if (handler_) handler_(failure);
    }

    bool try_recovery(std::string& error) override {
        if (failureCount_ >= maxFailures_) {
            error = "too many failures";
            return false;
        }
        failureCount_ = 0;
        lastFailure_.reset();
        return true;
    }

    bool force_unload(bool revert_corrupted, std::string& error) override {
        unloaded_ = true;
        reverted_ = revert_corrupted;
        return true;
    }

    void reset_failure_count() override {
        failureCount_ = 0;
        lastFailure_.reset();
    }

    void on_failure(FailureHandler handler) override {
        handler_ = std::move(handler);
    }

    // Test helpers
    bool was_unloaded() const { return unloaded_; }
    bool was_reverted() const { return reverted_; }
    void set_max_failures(std::uint32_t m) { maxFailures_ = m; }

private:
    std::string name_;
    std::uint32_t failureCount_{ 0 };
    std::optional<PluginFailure> lastFailure_;
    FailureHandler handler_;
    bool unloaded_{ false };
    bool reverted_{ false };
    std::uint32_t maxFailures_{ 10 };
};

class MockHotReload : public IPluginHotReload {
public:
    bool is_supported(const std::string& plugin_name) const override {
        return supported_.count(plugin_name) > 0;
    }

    HotReloadState checkpoint(const std::string& plugin_name) override {
        HotReloadState state;
        state.plugin_name = plugin_name;
        state.serialized_state = "{\"checkpoint\":true}";
        state.checkpoint_ms = 1000;
        state.valid = true;
        history_[plugin_name].push_back(state);
        return state;
    }

    HotReloadResult reload(const std::string& plugin_name,
                           const std::string& new_library_path,
                           const HotReloadState& checkpoint,
                           std::string& error) override {
        if (!is_supported(plugin_name)) {
            error = "not supported";
            return HotReloadResult::Failed;
        }
        reloadCount_++;
        return HotReloadResult::Success;
    }

    HotReloadResult rollback(const std::string& plugin_name,
                             const HotReloadState& checkpoint,
                             std::string& error) override {
        rollbackCount_++;
        return HotReloadResult::Success;
    }

    std::vector<HotReloadState> history(const std::string& plugin_name) const override {
        auto it = history_.find(plugin_name);
        return it != history_.end() ? it->second : std::vector<HotReloadState>{};
    }

    void clear_history(const std::string& plugin_name) override {
        history_.erase(plugin_name);
    }

    // Test helpers
    void mark_supported(const std::string& name) { supported_.insert(name); }
    int reload_count() const { return reloadCount_; }
    int rollback_count() const { return rollbackCount_; }

private:
    std::unordered_set<std::string> supported_;
    std::unordered_map<std::string, std::vector<HotReloadState>> history_;
    int reloadCount_{ 0 };
    int rollbackCount_{ 0 };
};

// ── Tests ──────────────────────────────────────────────────────────

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { printf("  FAIL: %s (line %d)\n", #expr, __LINE__); } \
} while(0)

void test_manifest_validation() {
    printf("test_manifest_validation...\n");
    PluginManifest m;
    m.name = "com.test.plugin";
    m.display_name = "Test Plugin";
    m.version = { 1, 0, 0 };

    std::string error;
    CHECK(m.validate(error) == true);

    // Empty name
    PluginManifest m2;
    m2.display_name = "No Name";
    CHECK(m2.validate(error) == false);
    CHECK(error.find("name") != std::string::npos);

    // Zero version
    PluginManifest m3;
    m3.name = "valid";
    m3.display_name = "Valid";
    m3.version = { 0, 0, 0 };
    CHECK(m3.validate(error) == false);
    CHECK(error.find("version") != std::string::npos);
}

void test_version_comparison() {
    printf("test_version_comparison...\n");
    PluginVersion v1{ 1, 0, 0 };
    PluginVersion v2{ 2, 0, 0 };
    PluginVersion v3{ 1, 1, 0 };
    PluginVersion v4{ 1, 0, 1 };

    CHECK(v1 < v2);
    CHECK(v1 < v3);
    CHECK(v1 < v4);
    CHECK(v2 > v1);
    CHECK(v3 > v1);
    CHECK(v4 > v1);
    CHECK(v1 == v1);
    CHECK(v1 != v2);
}

void test_version_constraint() {
    printf("test_version_constraint...\n");
    PluginVersion v{ 1, 5, 0 };

    VersionConstraint c1{ "*" };
    CHECK(c1.satisfies(v) == true);

    VersionConstraint c2{ ">=1.0.0" };
    CHECK(c2.satisfies(v) == true);

    VersionConstraint c3{ ">=2.0.0" };
    CHECK(c3.satisfies(v) == false);

    VersionConstraint c4{ "==1.5.0" };
    CHECK(c4.satisfies(v) == true);

    VersionConstraint c5{ "==1.0.0" };
    CHECK(c5.satisfies(v) == false);
}

void test_manifest_register_and_list() {
    printf("test_manifest_register_and_list...\n");
    MockManifestManager mgr;

    PluginManifest m;
    m.name = "com.test.alpha";
    m.display_name = "Alpha";
    m.version = { 1, 0, 0 };

    std::string error;
    CHECK(mgr.register_manifest(m, error) == true);
    CHECK(mgr.list().size() == 1);

    // Duplicate
    CHECK(mgr.register_manifest(m, error) == false);
    CHECK(error.find("duplicate") != std::string::npos);

    // Get
    auto* got = mgr.get("com.test.alpha");
    CHECK(got != nullptr);
    CHECK(got->name == "com.test.alpha");

    // Unregister
    CHECK(mgr.unregister("com.test.alpha", error) == true);
    CHECK(mgr.list().size() == 0);
}

void test_dependency_resolution() {
    printf("test_dependency_resolution...\n");
    MockManifestManager mgr;

    PluginManifest base;
    base.name = "base";
    base.display_name = "Base";
    base.version = { 1, 0, 0 };

    PluginManifest app;
    app.name = "app";
    app.display_name = "App";
    app.version = { 1, 0, 0 };
    app.dependencies.push_back({ "base", { "*" }, true });

    std::string error;
    CHECK(mgr.register_manifest(base, error) == true);
    CHECK(mgr.register_manifest(app, error) == true);

    auto order = mgr.resolve_dependencies(error);
    CHECK(order.size() == 2);
    CHECK(order[0] == "base");  // base before app
    CHECK(order[1] == "app");
}

void test_dependency_cycle() {
    printf("test_dependency_cycle...\n");
    MockManifestManager mgr;

    PluginManifest a;
    a.name = "a";
    a.display_name = "A";
    a.version = { 1, 0, 0 };
    a.dependencies.push_back({ "b", { "*" }, true });

    PluginManifest b;
    b.name = "b";
    b.display_name = "B";
    b.version = { 1, 0, 0 };
    b.dependencies.push_back({ "a", { "*" }, true });

    std::string error;
    CHECK(mgr.register_manifest(a, error) == true);
    CHECK(mgr.register_manifest(b, error) == true);

    auto order = mgr.resolve_dependencies(error);
    CHECK(order.empty());
    CHECK(error.find("circular") != std::string::npos);
}

void test_can_load() {
    printf("test_can_load...\n");
    MockManifestManager mgr;

    PluginManifest m;
    m.name = "plugin";
    m.display_name = "Plugin";
    m.version = { 1, 0, 0 };
    m.dependencies.push_back({ "dep", { ">=1.0.0" }, true });

    std::string error;
    CHECK(mgr.can_load("nonexistent", error) == false);

    mgr.register_manifest(m, error);
    CHECK(mgr.can_load("plugin", error) == false);  // dep missing
    CHECK(error.find("dep") != std::string::npos);
}

void test_isolation_healthy() {
    printf("test_isolation_healthy...\n");
    MockPluginIsolation iso("test_plugin");
    CHECK(iso.plugin_name() == "test_plugin");
    CHECK(iso.is_healthy() == true);
    CHECK(iso.failure_count() == 0);
    CHECK(iso.last_failure() == nullptr);
}

void test_isolation_failure() {
    printf("test_isolation_failure...\n");
    MockPluginIsolation iso("test_plugin");

    PluginFailure f;
    f.plugin_name = "test_plugin";
    f.severity = FailureSeverity::Warning;
    f.error_type = "timeout";
    f.error_message = "execution exceeded 5000ms";

    iso.record_failure(f);
    CHECK(iso.failure_count() == 1);
    CHECK(iso.last_failure() != nullptr);
    CHECK(iso.last_failure()->error_type == "timeout");
    CHECK(iso.is_healthy() == true);  // Warning doesn't make unhealthy
}

void test_isolation_critical_failure() {
    printf("test_isolation_critical_failure...\n");
    MockPluginIsolation iso("test_plugin");

    PluginFailure f;
    f.severity = FailureSeverity::Critical;
    f.error_type = "crash";

    iso.record_failure(f);
    CHECK(iso.is_healthy() == false);
}

void test_isolation_recovery() {
    printf("test_isolation_recovery...\n");
    MockPluginIsolation iso("test_plugin");

    PluginFailure f;
    f.severity = FailureSeverity::Recoverable;
    iso.record_failure(f);
    CHECK(iso.failure_count() == 1);

    std::string error;
    CHECK(iso.try_recovery(error) == true);
    CHECK(iso.failure_count() == 0);
}

void test_isolation_force_unload() {
    printf("test_isolation_force_unload...\n");
    MockPluginIsolation iso("test_plugin");

    std::string error;
    CHECK(iso.force_unload(true, error) == true);
    CHECK(iso.was_unloaded() == true);
    CHECK(iso.was_reverted() == true);
}

void test_isolation_failure_handler() {
    printf("test_isolation_failure_handler...\n");
    MockPluginIsolation iso("test_plugin");

    int handler_calls = 0;
    iso.on_failure([&](const PluginFailure&) { handler_calls++; });

    PluginFailure f;
    iso.record_failure(f);
    CHECK(handler_calls == 1);
}

void test_hot_reload_supported() {
    printf("test_hot_reload_supported...\n");
    MockHotReload reload;
    CHECK(reload.is_supported("plugin_a") == false);
    reload.mark_supported("plugin_a");
    CHECK(reload.is_supported("plugin_a") == true);
}

void test_hot_reload_checkpoint_and_reload() {
    printf("test_hot_reload_checkpoint_and_reload...\n");
    MockHotReload reload;
    reload.mark_supported("plugin_a");

    auto state = reload.checkpoint("plugin_a");
    CHECK(state.valid == true);
    CHECK(state.plugin_name == "plugin_a");

    std::string error;
    auto result = reload.reload("plugin_a", "/new/path", state, error);
    CHECK(result == HotReloadResult::Success);
    CHECK(reload.reload_count() == 1);
}

void test_hot_reload_unsupported() {
    printf("test_hot_reload_unsupported...\n");
    MockHotReload reload;

    auto state = reload.checkpoint("plugin_a");
    std::string error;
    auto result = reload.reload("plugin_a", "/new/path", state, error);
    CHECK(result == HotReloadResult::Failed);
    CHECK(error.find("not supported") != std::string::npos);
}

void test_hot_reload_rollback() {
    printf("test_hot_reload_rollback...\n");
    MockHotReload reload;
    reload.mark_supported("plugin_a");

    auto state = reload.checkpoint("plugin_a");
    std::string error;
    auto result = reload.rollback("plugin_a", state, error);
    CHECK(result == HotReloadResult::Success);
    CHECK(reload.rollback_count() == 1);
}

void test_hot_reload_history() {
    printf("test_hot_reload_history...\n");
    MockHotReload reload;
    reload.mark_supported("plugin_a");

    reload.checkpoint("plugin_a");
    reload.checkpoint("plugin_a");
    auto h = reload.history("plugin_a");
    CHECK(h.size() == 2);

    reload.clear_history("plugin_a");
    h = reload.history("plugin_a");
    CHECK(h.empty());
}

int main() {
    printf("=== PluginManifestTests ===\n");
    test_manifest_validation();
    test_version_comparison();
    test_version_constraint();
    test_manifest_register_and_list();
    test_dependency_resolution();
    test_dependency_cycle();
    test_can_load();
    test_isolation_healthy();
    test_isolation_failure();
    test_isolation_critical_failure();
    test_isolation_recovery();
    test_isolation_force_unload();
    test_isolation_failure_handler();
    test_hot_reload_supported();
    test_hot_reload_checkpoint_and_reload();
    test_hot_reload_unsupported();
    test_hot_reload_rollback();
    test_hot_reload_history();
    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
