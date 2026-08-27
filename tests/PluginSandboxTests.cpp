// PluginSandboxTests.cpp — headless gate for plugin isolation + type registration.
// Tests: sandbox permissions, type registry CRUD, deterministic ordering,
// plugin-scoped queries, and edge cases (empty ids, duplicates, unknowns).
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "engine/plugins/IPluginSandbox.hpp"
#include "engine/plugins/IPluginTypeRegistry.hpp"

using namespace Engine::Plugins;

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(expr, msg) do { \
    ++g_checks; \
    if (!(expr)) { \
        ++g_fails; \
        std::cerr << "  FAIL: " << msg << " (" #expr ")\n"; \
    } \
} while(0)

// ---------------------------------------------------------------------------
// 1. Sandbox: basic permission checks
// ---------------------------------------------------------------------------
static void test_sandbox_basics() {
    std::cerr << "[PS] sandbox basics\n";
    PluginPermissions perms;
    perms.required = {"filesystem:read", "network:connect"};
    perms.granted  = {"filesystem:read"};  // network denied

    PluginSandbox sb("MyPlugin", perms);

    CHECK(sb.plugin_name() == "MyPlugin", "plugin name");
    CHECK(sb.check("filesystem:read") == true,  "granted perm");
    CHECK(sb.check("network:connect") == false, "denied perm");
    CHECK(sb.check("process:spawn")   == false, "undeclared perm");
    CHECK(sb.permissions().all_granted() == false, "not all granted");
    std::cerr << "[PS] sandbox basics OK\n";
}

// ---------------------------------------------------------------------------
// 2. Sandbox: all permissions granted
// ---------------------------------------------------------------------------
static void test_sandbox_all_granted() {
    std::cerr << "[PS] sandbox all granted\n";
    PluginPermissions perms;
    perms.required = {"assets:read", "assets:write"};
    perms.granted  = {"assets:read", "assets:write"};

    PluginSandbox sb("AssetPlugin", perms);
    CHECK(sb.check_all({"assets:read", "assets:write"}) == true, "all granted");
    CHECK(sb.permissions().all_granted() == true, "all_granted true");
    std::cerr << "[PS] sandbox all granted OK\n";
}

// ---------------------------------------------------------------------------
// 3. Type registry: register + query
// ---------------------------------------------------------------------------
static void test_type_registry_basics() {
    std::cerr << "[PS] type registry basics\n";
    PluginTypeRegistry reg;

    PluginRegistration r1;
    r1.kind = PluginRegistrationKind::Component;
    r1.id = "my_plugin.health_bar";
    r1.name = "Health Bar";
    r1.description = "A health bar component";
    r1.pluginName = "MyPlugin";
    r1.version = "1.0.0";

    std::string err;
    CHECK(reg.register_item(r1, &err) == true, "register succeeds");
    CHECK(err.empty(), "no error on success");
    CHECK(reg.count() == 1, "count is 1");
    CHECK(reg.is_registered("my_plugin.health_bar"), "is registered");

    const auto* got = reg.get("my_plugin.health_bar");
    CHECK(got != nullptr, "get returns non-null");
    CHECK(got->name == "Health Bar", "name matches");
    CHECK(got->kind == PluginRegistrationKind::Component, "kind matches");
    CHECK(got->pluginName == "MyPlugin", "plugin name matches");
    std::cerr << "[PS] type registry basics OK\n";
}

// ---------------------------------------------------------------------------
// 4. Type registry: duplicate rejection
// ---------------------------------------------------------------------------
static void test_type_registry_duplicates() {
    std::cerr << "[PS] type registry duplicates\n";
    PluginTypeRegistry reg;

    PluginRegistration r;
    r.kind = PluginRegistrationKind::Asset;
    r.id = "my_plugin.texture";
    r.name = "Texture";
    r.pluginName = "MyPlugin";

    CHECK(reg.register_item(r) == true, "first register");
    CHECK(reg.register_item(r) == false, "duplicate rejected");
    CHECK(reg.count() == 1, "count stays 1");
    std::cerr << "[PS] type registry duplicates OK\n";
}

// ---------------------------------------------------------------------------
// 5. Type registry: empty id rejection
// ---------------------------------------------------------------------------
static void test_type_registry_empty_id() {
    std::cerr << "[PS] type registry empty id\n";
    PluginTypeRegistry reg;

    PluginRegistration r;
    r.kind = PluginRegistrationKind::McpTool;
    r.id = "";
    r.name = "Bad Tool";
    r.pluginName = "MyPlugin";

    std::string err;
    CHECK(reg.register_item(r, &err) == false, "empty id rejected");
    CHECK(err.find("empty") != std::string::npos, "error mentions empty");
    CHECK(reg.count() == 0, "count stays 0");
    std::cerr << "[PS] type registry empty id OK\n";
}

// ---------------------------------------------------------------------------
// 6. Type registry: unregister
// ---------------------------------------------------------------------------
static void test_type_registry_unregister() {
    std::cerr << "[PS] type registry unregister\n";
    PluginTypeRegistry reg;

    PluginRegistration r;
    r.kind = PluginRegistrationKind::Panel;
    r.id = "my_plugin.settings";
    r.name = "Settings Panel";
    r.pluginName = "MyPlugin";

    CHECK(reg.register_item(r) == true, "register");

    std::string err;
    CHECK(reg.unregister("my_plugin.settings", &err) == true, "unregister");
    CHECK(reg.is_registered("my_plugin.settings") == false, "not registered");
    CHECK(reg.count() == 0, "count is 0");

    CHECK(reg.unregister("nonexistent", &err) == false, "unregister unknown");
    CHECK(err.find("unknown") != std::string::npos, "error mentions unknown");
    std::cerr << "[PS] type registry unregister OK\n";
}

// ---------------------------------------------------------------------------
// 7. Type registry: filter by kind
// ---------------------------------------------------------------------------
static void test_type_registry_filter() {
    std::cerr << "[PS] type registry filter\n";
    PluginTypeRegistry reg;

    PluginRegistration r1; r1.kind = PluginRegistrationKind::Component;
    r1.id = "a.comp"; r1.pluginName = "P1";
    PluginRegistration r2; r2.kind = PluginRegistrationKind::Asset;
    r2.id = "b.asset"; r2.pluginName = "P1";
    PluginRegistration r3; r3.kind = PluginRegistrationKind::Component;
    r3.id = "c.comp"; r3.pluginName = "P2";

    reg.register_item(r1);
    reg.register_item(r2);
    reg.register_item(r3);

    auto components = reg.list(PluginRegistrationKind::Component);
    CHECK(components.size() == 2, "2 components");
    CHECK(components[0].id == "a.comp", "first component");
    CHECK(components[1].id == "c.comp", "second component");

    auto assets = reg.list(PluginRegistrationKind::Asset);
    CHECK(assets.size() == 1, "1 asset");
    CHECK(assets[0].id == "b.asset", "the asset");

    auto all = reg.list();
    CHECK(all.size() == 3, "all 3");
    std::cerr << "[PS] type registry filter OK\n";
}

// ---------------------------------------------------------------------------
// 8. Type registry: list by plugin
// ---------------------------------------------------------------------------
static void test_type_registry_by_plugin() {
    std::cerr << "[PS] type registry by plugin\n";
    PluginTypeRegistry reg;

    PluginRegistration r1; r1.kind = PluginRegistrationKind::Component;
    r1.id = "p1.a"; r1.pluginName = "Plugin1";
    PluginRegistration r2; r2.kind = PluginRegistrationKind::Component;
    r2.id = "p2.a"; r2.pluginName = "Plugin2";
    PluginRegistration r3; r3.kind = PluginRegistrationKind::Asset;
    r3.id = "p1.b"; r3.pluginName = "Plugin1";

    reg.register_item(r1);
    reg.register_item(r2);
    reg.register_item(r3);

    auto p1 = reg.list_by_plugin("Plugin1");
    CHECK(p1.size() == 2, "Plugin1 has 2 items");
    CHECK(p1[0].id == "p1.a", "first from Plugin1");
    CHECK(p1[1].id == "p1.b", "second from Plugin1");

    auto p2 = reg.list_by_plugin("Plugin2");
    CHECK(p2.size() == 1, "Plugin2 has 1 item");

    auto p3 = reg.list_by_plugin("Nonexistent");
    CHECK(p3.empty(), "nonexistent plugin");
    std::cerr << "[PS] type registry by plugin OK\n";
}

// ---------------------------------------------------------------------------
// 9. Type registry: deterministic insertion order
// ---------------------------------------------------------------------------
static void test_type_registry_order() {
    std::cerr << "[PS] type registry order\n";
    PluginTypeRegistry reg;

    for (int i = 0; i < 50; ++i) {
        PluginRegistration r;
        r.kind = static_cast<PluginRegistrationKind>(i % 4);
        r.id = "item_" + std::to_string(i);
        r.pluginName = "P";
        reg.register_item(r);
    }

    auto all = reg.list();
    CHECK(all.size() == 50, "50 items");
    for (std::size_t i = 0; i < all.size(); ++i) {
        CHECK(all[i].id == "item_" + std::to_string(i),
              "order preserved at " + std::to_string(i));
    }
    std::cerr << "[PS] type registry order OK\n";
}

// ---------------------------------------------------------------------------
// 10. Type registry: clear
// ---------------------------------------------------------------------------
static void test_type_registry_clear() {
    std::cerr << "[PS] type registry clear\n";
    PluginTypeRegistry reg;

    PluginRegistration r;
    r.kind = PluginRegistrationKind::Command;
    r.id = "cmd.test";
    r.pluginName = "P";
    reg.register_item(r);
    CHECK(reg.count() == 1, "has 1");

    reg.clear();
    CHECK(reg.count() == 0, "cleared to 0");
    CHECK(reg.is_registered("cmd.test") == false, "not registered after clear");
    std::cerr << "[PS] type registry clear OK\n";
}

// ---------------------------------------------------------------------------
// 11. Sandbox: empty permissions
// ---------------------------------------------------------------------------
static void test_sandbox_empty() {
    std::cerr << "[PS] sandbox empty\n";
    PluginPermissions perms;
    PluginSandbox sb("EmptyPlugin", perms);

    CHECK(sb.check("filesystem:read") == false, "no perms = denied");
    CHECK(sb.permissions().all_granted() == true, "all_granted vacuously true");
    CHECK(sb.check_all({}) == true, "check_all empty = true");
    std::cerr << "[PS] sandbox empty OK\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    test_sandbox_basics();
    test_sandbox_all_granted();
    test_sandbox_empty();
    test_type_registry_basics();
    test_type_registry_duplicates();
    test_type_registry_empty_id();
    test_type_registry_unregister();
    test_type_registry_filter();
    test_type_registry_by_plugin();
    test_type_registry_order();
    test_type_registry_clear();

    std::cerr << "\n[plugin_sandbox] " << g_checks << " checks, "
              << g_fails << " failures\n";
    return g_fails == 0 ? 0 : 1;
}
