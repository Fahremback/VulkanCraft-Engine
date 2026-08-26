// Gate for engine::editor::IContentBrowser (agente 2 §B) — the Content
// Browser navigation model. Headless: no editor, no window, no clock.

#include "engine/editor/IContentBrowser.hpp"

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

engine::editor::ContentBrowserDoc make_doc() {
    engine::editor::ContentBrowserDoc doc;
    doc.version = 1;
    const auto add = [&](const char* id, const char* name, const char* type,
                         const char* folder) {
        engine::editor::ContentAsset a;
        a.id = id; a.name = name; a.type = type; a.folder = folder;
        doc.assets.push_back(std::move(a));
    };
    add("tex.brick", "brick", "texture", "Textures");
    add("tex.grass", "grass", "texture", "Textures");
    add("mdl.player", "player", "model", "Models");
    add("mdl.cube", "cube", "model", "Models");
    add("snd.step", "step", "audio", "Audio");
    add("scn.main", "main", "scene", "Scenes");
    add("root.doc", "readme", "script", "");
    return doc;
}

void test_validate() {
    engine::editor::ContentBrowserDoc doc = make_doc();
    std::string err;
    CHECK(doc.validate(err), "valid doc validates");

    engine::editor::ContentBrowserDoc bad = doc;
    bad.version = 2;
    CHECK(!bad.validate(err), "wrong version refused");

    bad = doc;
    bad.assets[1].id.clear();
    CHECK(!bad.validate(err), "empty id refused");

    bad = doc;
    bad.assets[0].name.clear();
    CHECK(!bad.validate(err), "empty name refused");

    bad = doc;
    bad.assets[0].id = bad.assets[1].id;
    CHECK(!bad.validate(err), "duplicate id refused");
}

void test_json_round_trip() {
    engine::editor::ContentBrowserDoc doc = make_doc();
    std::string err;
    const std::string json = doc.to_json();
    engine::editor::ContentBrowserDoc restored;
    CHECK(restored.load_from_json(json, err), "round-trip load");
    CHECK(restored.to_json() == json, "bit-exact round-trip");

    CHECK(!restored.load_from_json("{broken", err), "malformed refused");
    CHECK(restored.assets.size() == 7, "failed load does not mutate");
}

void test_folders() {
    std::string err;
    auto cb = engine::editor::create_content_browser(make_doc(), err);
    CHECK(cb != nullptr, "index compiles");

    const auto tree = cb->folders();
    CHECK(tree.size() == 5, "5 folders (incl. root)");
    CHECK(tree[0].path.empty(), "root folder first (sorted)");
    CHECK(tree[0].assets.size() == 1 && tree[0].assets[0].id == "root.doc",
          "root holds the root asset");
    // Folders sorted: Audio, Models, Scenes, Textures (root "" first).
    CHECK(tree[1].path == "Audio", "Audio second");
    CHECK(tree[2].path == "Models", "Models third");
    CHECK(tree[2].assets.size() == 2, "Models has 2 assets");
    CHECK(tree[2].assets[0].name < tree[2].assets[1].name, "folder assets sorted by name");
    CHECK(tree[4].path == "Textures", "Textures last");
}

void test_search_and_type() {
    std::string err;
    auto cb = engine::editor::create_content_browser(make_doc(), err);

    auto hits = cb->search("tex");
    CHECK(hits.size() == 2, "search 'tex' finds Textures folder assets");
    for (const auto& h : hits) CHECK(h.folder == "Textures", "folder match");

    hits = cb->search("player");
    CHECK(hits.size() == 1 && hits[0].id == "mdl.player", "name match");

    hits = cb->search("SCENE");
    CHECK(hits.size() == 1 && hits[0].id == "scn.main", "case-insensitive");

    hits = cb->search("zzz");
    CHECK(hits.empty(), "no match empty");

    hits = cb->search("");
    CHECK(hits.size() == 7, "empty query returns all");

    auto textures = cb->by_type("texture");
    CHECK(textures.size() == 2, "texture type filter");
    auto scenes = cb->by_type("scene");
    CHECK(scenes.size() == 1 && scenes[0].id == "scn.main", "scene type filter");
    auto none = cb->by_type("nope");
    CHECK(none.empty(), "unknown type empty");
}

void test_selection() {
    std::string err;
    auto cb = engine::editor::create_content_browser(make_doc(), err);

    CHECK(!cb->has_selection(), "no selection initially");
    CHECK(!cb->select("nope"), "unknown id refused");
    CHECK(!cb->has_selection(), "refusal leaves selection empty");

    CHECK(cb->select("mdl.player"), "select model");
    CHECK(cb->has_selection(), "has selection");
    CHECK(cb->selection_id() == "mdl.player", "selection is the model");

    CHECK(cb->select("tex.brick"), "select another");
    CHECK(cb->selection_id() == "tex.brick", "selection replaced (one at a time)");

    cb->clear_selection();
    CHECK(!cb->has_selection(), "cleared");
}

void test_runtime_add() {
    std::string err;
    auto cb = engine::editor::create_content_browser(make_doc(), err);

    engine::editor::ContentAsset a;
    a.id = "mat.iron"; a.name = "iron"; a.type = "material"; a.folder = "Materials";
    CHECK(cb->add(a, err), "runtime add");
    CHECK(cb->size() == 8, "size grew");
    CHECK(cb->find("mat.iron") != nullptr, "findable after add");

    CHECK(!cb->add(a, err), "duplicate refused");
    CHECK(cb->size() == 8, "refused add does not mutate");

    engine::editor::ContentAsset empty;
    CHECK(!cb->add(empty, err), "empty id refused");
    CHECK(cb->size() == 8, "still 8 after refusals");
}

void test_determinism() {
    std::string err;
    auto a = engine::editor::create_content_browser(make_doc(), err);
    auto b = engine::editor::create_content_browser(make_doc(), err);

    CHECK(a->assets() == b->assets(), "assets equal cross-instance");
    CHECK(a->folders() == b->folders(), "folders equal cross-instance");
    for (const char* q : { "tex", "model", "s", "x" }) {
        CHECK(a->search(q) == b->search(q), "search deterministic");
    }
    CHECK(a->by_type("model") == b->by_type("model"), "type filter deterministic");
    a->select("scn.main"); b->select("scn.main");
    CHECK(a->to_json() == b->to_json(), "JSON bit-exact cross-instance");
}

}  // namespace

int main() {
    test_validate();
    test_json_round_trip();
    test_folders();
    test_search_and_type();
    test_selection();
    test_runtime_add();
    test_determinism();

    if (g_failures == 0) {
        std::printf("ALL PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
