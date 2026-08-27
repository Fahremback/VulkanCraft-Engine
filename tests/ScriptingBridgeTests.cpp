// ScriptingBridgeTests.cpp — testes headless para IScriptingBridge
// Verifica: criação, leitura/escrita de componentes, consulta,
// eventos, batch atômico, validação de permissões.
//
// Todos os testes são determinísticos e não requerem GPU, filesystem
// ou rede.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/scripting/IScriptingBridge.hpp"
#include "engine/plugins/IPluginManifest.hpp"

using namespace engine::scripting;

// ── Mock ECS implementation ────────────────────────────────────────

struct MockComponent {
    std::string type;
    std::string data;  // JSON
};

class MockEcsBridge : public IScriptingBridge, public IScriptingBridgeBatch {
public:
    explicit MockEcsBridge(std::string ctx) : contextId_(std::move(ctx)) {}

    const std::string& context_id() const override { return contextId_; }

    bool has_permission(BridgePermission perm) const override {
        for (const auto& [p, v] : permissions_) {
            if (p == perm) return v;
        }
        return false;
    }

    void grant_permission(BridgePermission perm) {
        for (auto& [p, v] : permissions_) {
            if (p == perm) { v = true; return; }
        }
        permissions_.emplace_back(perm, true);
    }
    void deny_permission(BridgePermission perm) {
        for (auto& [p, v] : permissions_) {
            if (p == perm) { v = false; return; }
        }
    }

    // ── Read ──

    BridgeResult read_component(const EntityHandle& entity,
                                const std::string& component_type) const override {
        if (!has_permission(BridgePermission::ReadComponents)) {
            return { false, "permission", "" };
        }
        auto it = entities_.find(entity);
        if (it == entities_.end()) {
            return { false, "not_found", "" };
        }
        for (const auto& c : it->second) {
            if (c.type == component_type) {
                return { true, "", c.data };
            }
        }
        return { false, "not_found", "" };
    }

    BridgeResult list_components(const EntityHandle& entity) const override {
        if (!has_permission(BridgePermission::ReadComponents)) {
            return { false, "permission", "" };
        }
        auto it = entities_.find(entity);
        if (it == entities_.end()) {
            return { false, "not_found", "" };
        }
        std::string json = "[";
        bool first = true;
        for (const auto& c : it->second) {
            if (!first) json += ",";
            json += "\"" + c.type + "\"";
            first = false;
        }
        json += "]";
        return { true, "", json };
    }

    // ── Write ──

    BridgeResult write_component(const EntityHandle& entity,
                                 const std::string& component_type,
                                 const std::string& data) override {
        if (!has_permission(BridgePermission::WriteComponents)) {
            return { false, "permission", "" };
        }
        if (data.empty()) {
            return { false, "invalid", "" };
        }
        auto& comps = entities_[entity];
        for (auto& c : comps) {
            if (c.type == component_type) {
                c.data = data;
                return { true, "", "" };
            }
        }
        comps.push_back({ component_type, data });
        return { true, "", "" };
    }

    BridgeResult remove_component(const EntityHandle& entity,
                                  const std::string& component_type) override {
        if (!has_permission(BridgePermission::WriteComponents)) {
            return { false, "permission", "" };
        }
        auto it = entities_.find(entity);
        if (it == entities_.end()) {
            return { false, "not_found", "" };
        }
        auto& comps = it->second;
        for (auto cit = comps.begin(); cit != comps.end(); ++cit) {
            if (cit->type == component_type) {
                comps.erase(cit);
                return { true, "", "" };
            }
        }
        return { false, "not_found", "" };
    }

    // ── Entities ──

    BridgeResult spawn_entity() override {
        if (!has_permission(BridgePermission::SpawnEntities)) {
            return { false, "permission", "" };
        }
        std::string id = "entity_" + std::to_string(nextId_++);
        entities_[id] = {};
        return { true, "", "\"" + id + "\"" };
    }

    BridgeResult destroy_entity(const EntityHandle& entity) override {
        if (!has_permission(BridgePermission::DestroyEntities)) {
            return { false, "permission", "" };
        }
        if (entities_.erase(entity) == 0) {
            return { false, "not_found", "" };
        }
        return { true, "", "" };
    }

    BridgeResult query_entities(const EntityQuery& query) const override {
        if (!has_permission(BridgePermission::QueryEntities)) {
            return { false, "permission", "" };
        }
        std::string json = "[";
        bool first = true;
        std::uint32_t count = 0;
        for (const auto& [id, comps] : entities_) {
            if (query.limit > 0 && count >= query.limit) break;
            bool has_required = true;
            for (const auto& req : query.required_components) {
                bool found = false;
                for (const auto& c : comps) {
                    if (c.type == req) { found = true; break; }
                }
                if (!found) { has_required = false; break; }
            }
            if (!has_required) continue;
            bool has_excluded = false;
            for (const auto& exc : query.excluded_components) {
                for (const auto& c : comps) {
                    if (c.type == exc) { has_excluded = true; break; }
                }
                if (has_excluded) break;
            }
            if (has_excluded) continue;
            if (!first) json += ",";
            json += "\"" + id + "\"";
            first = false;
            count++;
        }
        json += "]";
        return { true, "", json };
    }

    // ── Events ──

    BridgeResult send_event(const std::string& event_type,
                            const std::string& data) override {
        if (!has_permission(BridgePermission::SendEvents)) {
            return { false, "permission", "" };
        }
        events_.push_back({ event_type, data });
        return { true, "", "" };
    }

    // ── Reflection ──

    BridgeResult list_component_types() const override {
        return { true, "", "[\"Transform\",\"Health\",\"Velocity\"]" };
    }

    BridgeResult get_component_schema(const std::string& component_type) const override {
        if (component_type == "Transform") {
            return { true, "", "{\"x\":\"number\",\"y\":\"number\",\"z\":\"number\"}" };
        }
        return { false, "not_found", "" };
    }

    // ── Batch ──

    BatchResult execute_batch(const std::vector<BatchOperation>& ops) override {
        // Collect results without applying
        BatchResult batch;
        batch.results.reserve(ops.size());
        for (const auto& op : ops) {
            BridgeResult r;
            switch (op.kind) {
                case BatchOperation::Kind::ReadComponent:
                    r = read_component(op.entity, op.component_type);
                    break;
                case BatchOperation::Kind::WriteComponent:
                    r = write_component(op.entity, op.component_type, op.data);
                    break;
                case BatchOperation::Kind::RemoveComponent:
                    r = remove_component(op.entity, op.component_type);
                    break;
                case BatchOperation::Kind::SpawnEntity:
                    r = spawn_entity();
                    break;
                case BatchOperation::Kind::DestroyEntity:
                    r = destroy_entity(op.entity);
                    break;
                case BatchOperation::Kind::SendEvent:
                    r = send_event(op.event_type, op.data);
                    break;
            }
            batch.results.push_back(r);
            if (!r.ok && batch.all_ok) {
                batch.all_ok = false;
                batch.error = r.error;
            }
        }
        return batch;
    }

    // ── Test helpers ──
    std::size_t event_count() const { return events_.size(); }
    const auto& last_event() const { return events_.back(); }

private:
    std::string contextId_;
    std::vector<std::pair<BridgePermission, bool>> permissions_;
    std::unordered_map<std::string, std::vector<MockComponent>> entities_;
    std::vector<std::pair<std::string, std::string>> events_;
    int nextId_{ 1 };
};

// ── Tests ──────────────────────────────────────────────────────────

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { printf("  FAIL: %s (line %d)\n", #expr, __LINE__); } \
} while(0)

void test_basic_creation() {
    printf("test_basic_creation...\n");
    MockEcsBridge bridge("test_ctx");
    CHECK(bridge.context_id() == "test_ctx");
    CHECK(bridge.has_permission(BridgePermission::ReadComponents) == false);
}

void test_permissions() {
    printf("test_permissions...\n");
    MockEcsBridge bridge("ctx");
    bridge.grant_permission(BridgePermission::ReadComponents);
    CHECK(bridge.has_permission(BridgePermission::ReadComponents) == true);
    CHECK(bridge.has_permission(BridgePermission::WriteComponents) == false);
    bridge.deny_permission(BridgePermission::ReadComponents);
    CHECK(bridge.has_permission(BridgePermission::ReadComponents) == false);
}

void test_spawn_and_read() {
    printf("test_spawn_and_read...\n");
    MockEcsBridge bridge("ctx");
    bridge.grant_permission(BridgePermission::SpawnEntities);
    bridge.grant_permission(BridgePermission::ReadComponents);

    auto r = bridge.spawn_entity();
    CHECK(r.ok == true);
    CHECK(r.data.find("entity_") != std::string::npos);

    // Read non-existent component
    auto r2 = bridge.read_component("entity_1", "Transform");
    CHECK(r2.ok == false);
    CHECK(r2.error == "not_found");
}

void test_write_and_read_component() {
    printf("test_write_and_read_component...\n");
    MockEcsBridge bridge("ctx");
    bridge.grant_permission(BridgePermission::SpawnEntities);
    bridge.grant_permission(BridgePermission::ReadComponents);
    bridge.grant_permission(BridgePermission::WriteComponents);

    bridge.spawn_entity();
    auto w = bridge.write_component("entity_1", "Health", "{\"hp\":100}");
    CHECK(w.ok == true);

    auto r = bridge.read_component("entity_1", "Health");
    CHECK(r.ok == true);
    CHECK(r.data == "{\"hp\":100}");

    // Update
    w = bridge.write_component("entity_1", "Health", "{\"hp\":50}");
    CHECK(w.ok == true);
    r = bridge.read_component("entity_1", "Health");
    CHECK(r.ok == true);
    CHECK(r.data == "{\"hp\":50}");
}

void test_remove_component() {
    printf("test_remove_component...\n");
    MockEcsBridge bridge("ctx");
    bridge.grant_permission(BridgePermission::SpawnEntities);
    bridge.grant_permission(BridgePermission::ReadComponents);
    bridge.grant_permission(BridgePermission::WriteComponents);

    bridge.spawn_entity();
    bridge.write_component("entity_1", "Health", "{\"hp\":100}");

    auto r = bridge.remove_component("entity_1", "Health");
    CHECK(r.ok == true);

    auto r2 = bridge.read_component("entity_1", "Health");
    CHECK(r2.ok == false);
    CHECK(r2.error == "not_found");
}

void test_destroy_entity() {
    printf("test_destroy_entity...\n");
    MockEcsBridge bridge("ctx");
    bridge.grant_permission(BridgePermission::SpawnEntities);
    bridge.grant_permission(BridgePermission::DestroyEntities);
    bridge.grant_permission(BridgePermission::ReadComponents);

    bridge.spawn_entity();
    auto r = bridge.destroy_entity("entity_1");
    CHECK(r.ok == true);

    auto r2 = bridge.read_component("entity_1", "Health");
    CHECK(r2.ok == false);
}

void test_query_entities() {
    printf("test_query_entities...\n");
    MockEcsBridge bridge("ctx");
    bridge.grant_permission(BridgePermission::SpawnEntities);
    bridge.grant_permission(BridgePermission::WriteComponents);
    bridge.grant_permission(BridgePermission::QueryEntities);

    bridge.spawn_entity();
    bridge.write_component("entity_1", "Health", "{\"hp\":100}");
    bridge.spawn_entity();
    bridge.write_component("entity_2", "Velocity", "{\"x\":1}");

    EntityQuery q;
    q.required_components.push_back("Health");
    auto r = bridge.query_entities(q);
    CHECK(r.ok == true);
    CHECK(r.data.find("entity_1") != std::string::npos);
    CHECK(r.data.find("entity_2") == std::string::npos);
}

void test_permission_denied() {
    printf("test_permission_denied...\n");
    MockEcsBridge bridge("ctx");
    // No permissions granted

    auto r = bridge.read_component("x", "Y");
    CHECK(r.ok == false);
    CHECK(r.error == "permission");

    r = bridge.write_component("x", "Y", "{}");
    CHECK(r.ok == false);

    r = bridge.spawn_entity();
    CHECK(r.ok == false);
}

void test_events() {
    printf("test_events...\n");
    MockEcsBridge bridge("ctx");
    bridge.grant_permission(BridgePermission::SendEvents);

    auto r = bridge.send_event("player_died", "{\"id\":42}");
    CHECK(r.ok == true);
    CHECK(bridge.event_count() == 1);
    CHECK(bridge.last_event().first == "player_died");
    CHECK(bridge.last_event().second == "{\"id\":42}");
}

void test_reflection() {
    printf("test_reflection...\n");
    MockEcsBridge bridge("ctx");

    auto r = bridge.list_component_types();
    CHECK(r.ok == true);
    CHECK(r.data.find("Transform") != std::string::npos);

    auto r2 = bridge.get_component_schema("Transform");
    CHECK(r2.ok == true);
    CHECK(r2.data.find("number") != std::string::npos);

    auto r3 = bridge.get_component_schema("Unknown");
    CHECK(r3.ok == false);
}

void test_list_components() {
    printf("test_list_components...\n");
    MockEcsBridge bridge("ctx");
    bridge.grant_permission(BridgePermission::SpawnEntities);
    bridge.grant_permission(BridgePermission::ReadComponents);
    bridge.grant_permission(BridgePermission::WriteComponents);

    bridge.spawn_entity();
    bridge.write_component("entity_1", "Health", "{\"hp\":100}");
    bridge.write_component("entity_1", "Transform", "{\"x\":0}");

    auto r = bridge.list_components("entity_1");
    CHECK(r.ok == true);
    CHECK(r.data.find("Health") != std::string::npos);
    CHECK(r.data.find("Transform") != std::string::npos);
}

void test_batch() {
    printf("test_batch...\n");
    MockEcsBridge bridge("ctx");
    bridge.grant_permission(BridgePermission::SpawnEntities);
    bridge.grant_permission(BridgePermission::ReadComponents);
    bridge.grant_permission(BridgePermission::WriteComponents);

    std::vector<BatchOperation> ops;
    ops.push_back({ BatchOperation::Kind::SpawnEntity, "", "", "", "" });
    ops.push_back({ BatchOperation::Kind::WriteComponent, "entity_1", "Health", "{\"hp\":100}", "" });

    auto batch = bridge.execute_batch(ops);
    CHECK(batch.all_ok == true);
    CHECK(batch.results.size() == 2);
    CHECK(batch.results[0].ok == true);
    CHECK(batch.results[1].ok == true);
}

void test_version_parse() {
    printf("test_version_parse...\n");
    using engine::plugins::PluginVersion;
    auto v = PluginVersion::parse("1.2.3");
    CHECK(v.major == 1);
    CHECK(v.minor == 2);
    CHECK(v.patch == 3);
    CHECK(v.to_string() == "1.2.3");

    auto v2 = PluginVersion::parse("2.0.0");
    CHECK(v.compare(v2) < 0);
    CHECK(v2.compare(v) > 0);
    CHECK(v.compare(v) == 0);
}

int main() {
    printf("=== ScriptingBridgeTests ===\n");
    test_basic_creation();
    test_permissions();
    test_spawn_and_read();
    test_write_and_read_component();
    test_remove_component();
    test_destroy_entity();
    test_query_entities();
    test_permission_denied();
    test_events();
    test_reflection();
    test_list_components();
    test_batch();
    test_version_parse();
    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
