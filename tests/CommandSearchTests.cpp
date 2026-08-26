// Gate for engine::editor::ICommandSearch (agente 2 §B) — the data-driven
// ranked command palette / global-search backbone. Headless: no editor, no
// window, no clock.

#include "engine/editor/ICommandSearch.hpp"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d — %s\n", __FILE__, __LINE__, msg); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

engine::editor::CommandIndexDoc make_doc() {
    engine::editor::CommandIndexDoc doc;
    doc.version = 1;
    {
        engine::editor::CommandEntry e;
        e.id = "scene.save"; e.label = "Save Scene"; e.category = "Scene";
        e.keywords = { "save", "write" }; e.action = "scene.save";
        doc.entries.push_back(e);
    }
    {
        engine::editor::CommandEntry e;
        e.id = "scene.open"; e.label = "Open Scene"; e.category = "Scene";
        e.keywords = { "open", "load" }; e.action = "scene.open";
        doc.entries.push_back(e);
    }
    {
        engine::editor::CommandEntry e;
        e.id = "build.game"; e.label = "Build / Export Game"; e.category = "Build";
        e.keywords = { "exe", "package" }; e.action = "build.game";
        doc.entries.push_back(e);
    }
    {
        engine::editor::CommandEntry e;
        e.id = "asset.import"; e.label = "Import Asset"; e.category = "Asset";
        e.keywords = { "import" }; e.action = "asset.import";
        doc.entries.push_back(e);
    }
    {
        engine::editor::CommandEntry e;
        e.id = "asset.refresh"; e.label = "Refresh Assets"; e.category = "Asset";
        e.keywords = { "reload", "refresh" }; e.action = "asset.refresh";
        doc.entries.push_back(e);
    }
    return doc;
}

void test_validate() {
    engine::editor::CommandIndexDoc doc = make_doc();
    std::string err;
    CHECK(doc.validate(err), "valid doc validates");

    engine::editor::CommandIndexDoc bad = doc;
    bad.version = 2;
    CHECK(!bad.validate(err), "wrong version refused");

    bad = doc;
    bad.entries[1].id.clear();
    CHECK(!bad.validate(err), "empty id refused");

    bad = doc;
    bad.entries[0].label.clear();
    CHECK(!bad.validate(err), "empty label refused");

    bad = doc;
    bad.entries[0].id = bad.entries[1].id;
    CHECK(!bad.validate(err), "duplicate id refused");
}

void test_json_round_trip() {
    engine::editor::CommandIndexDoc doc = make_doc();
    std::string err;

    const std::string json = doc.to_json();
    engine::editor::CommandIndexDoc restored;
    CHECK(restored.load_from_json(json, err), "round-trip load");
    CHECK(restored.to_json() == json, "bit-exact round-trip");

    CHECK(!restored.load_from_json("{broken", err), "malformed refused");
    CHECK(restored.entries.size() == 5, "failed load does not mutate");

    engine::editor::CommandIndexDoc dup = make_doc();
    dup.entries.push_back(dup.entries[0]);
    std::string dupJson = dup.to_json();
    engine::editor::CommandIndexDoc target = make_doc();
    CHECK(!target.load_from_json(dupJson, err), "duplicate id refused on load");
    CHECK(target.entries.size() == 5, "rejected load leaves doc untouched");
}

void test_search_ranking() {
    engine::editor::CommandIndexDoc doc = make_doc();
    std::string err;
    auto cs = engine::editor::create_command_search(doc, err);
    CHECK(cs != nullptr, "index compiles");

    // "save" — label prefix on Save Scene (100); the only entry with "save".
    auto hits = cs->search("save");
    CHECK(!hits.empty(), "save finds results");
    CHECK(hits[0].id == "scene.save", "label prefix ranks first");
    CHECK(hits[0].score == 100, "prefix score is 100");
    CHECK(hits.size() == 1, "save matches only scene.save");

    // "scene" — word-boundary match (60) on "Save Scene"/"Open Scene"
    // (the query is a full word in both labels, so 60, not substring 30).
    // Tie broken by (category,id): scene.open before scene.save.
    hits = cs->search("scene");
    CHECK(hits.size() == 2, "scene finds 2");
    for (const auto& h : hits) {
        CHECK(h.score == 60, "scene scoring is word-boundary 60");
    }
    CHECK(hits[0].id == "scene.open", "tie broken by id asc");
    CHECK(hits[1].id == "scene.save", "second scene hit");

    // "asset" — word-boundary (60) on "Import Asset" and "Refresh Assets"
    // (query is the last/first word of both labels).
    hits = cs->search("asset");
    CHECK(hits.size() == 2, "asset finds 2");
    for (const auto& h : hits) {
        CHECK(h.score == 60, "asset scoring is word-boundary 60");
    }
    CHECK(hits[0].id == "asset.import", "asset tie broken by id asc");

    // "exp" — word-boundary prefix of "Export" in "Build / Export Game" (60).
    hits = cs->search("exp");
    CHECK(!hits.empty(), "exp finds the export command");
    CHECK(hits[0].id == "build.game", "export match leads");
    CHECK(hits[0].score == 60, "word-boundary score is 60");

    // keyword-only: "package" hits build.game via keyword (20).
    hits = cs->search("package");
    CHECK(hits.size() == 1 && hits[0].id == "build.game", "keyword-only match");
    CHECK(hits[0].score == 20, "keyword score is 20");

    // no match -> empty, deterministic.
    hits = cs->search("zzznope");
    CHECK(hits.empty(), "no match is empty");

    // case-insensitive.
    hits = cs->search("SAVE");
    CHECK(!hits.empty() && hits[0].id == "scene.save", "case-insensitive");
}

void test_empty_query_returns_declaration_order() {
    engine::editor::CommandIndexDoc doc = make_doc();
    std::string err;
    auto cs = engine::editor::create_command_search(doc, err);

    auto hits = cs->search("");
    CHECK(hits.size() == 5, "empty query returns all");
    CHECK(hits[0].id == "scene.save", "declaration order preserved");
    CHECK(hits[4].id == "asset.refresh", "last declared is last");
    for (const auto& h : hits) CHECK(h.score == 0, "no score for empty query");
}

void test_determinism() {
    engine::editor::CommandIndexDoc doc = make_doc();
    std::string err;
    auto a = engine::editor::create_command_search(doc, err);
    auto b = engine::editor::create_command_search(doc, err);

    for (const char* q : { "s", "sc", "save", "asset", "exp", "game", "x" }) {
        const auto ha = a->search(q);
        const auto hb = b->search(q);
        CHECK(ha == hb, "bit-exact determinism cross-instance");
        CHECK(ha.size() == hb.size(), "same hit count");
    }
}

void test_runtime_add() {
    engine::editor::CommandIndexDoc doc = make_doc();
    std::string err;
    auto cs = engine::editor::create_command_search(doc, err);

    engine::editor::CommandEntry e;
    e.id = "edit.undo"; e.label = "Undo"; e.category = "Edit";
    e.keywords = { "undo" }; e.action = "edit.undo";
    CHECK(cs->add(e, err), "runtime add");
    CHECK(cs->size() == 6, "size grew");
    CHECK(cs->find("edit.undo") != nullptr, "findable after add");

    auto hits = cs->search("undo");
    CHECK(!hits.empty() && hits[0].id == "edit.undo", "new entry searchable");

    engine::editor::CommandEntry dup = e;
    CHECK(!cs->add(dup, err), "duplicate refused at runtime");
    CHECK(cs->size() == 6, "refused add does not mutate");

    engine::editor::CommandEntry empty;
    CHECK(!cs->add(empty, err), "empty id refused");
    CHECK(cs->size() == 6, "still 6 after refusals");
}

}  // namespace

int main() {
    test_validate();
    test_json_round_trip();
    test_search_ranking();
    test_empty_query_returns_declaration_order();
    test_determinism();
    test_runtime_add();

    if (g_failures == 0) {
        std::printf("ALL PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
