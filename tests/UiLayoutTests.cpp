// UiLayoutTests — headless coverage for the public data-driven UI layout +
// data binding contract (engine/ui/ILayout.hpp, adapter UiLayout.cpp). Mirrors
// the ActionMapTests pattern: the contract compiles with public SDK headers
// only, the gate drives the deterministic resolver directly. No window, no
// GPU. Standalone main() with CHECK.

#include "engine/ui/ILayout.hpp"

#include <cstdlib>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace engine::ui;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "UiLayoutTests failure at line " << __LINE__ << ": " #condition "\n"; \
    return false; } } while (false)

namespace {

LayoutNode make_node(std::string id, double weight = 1.0,
                     LayoutDirection direction = LayoutDirection::Column) {
    LayoutNode n;
    n.id = std::move(id);
    n.weight = weight;
    n.direction = direction;
    return n;
}

bool near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

bool run_all() {
    std::string err;

    // ---- eval_binding: literals + store refs + arithmetic ----------------
    UiStore store;
    store["hp"] = UiValue{ UiValue::Kind::Number, 5.0, false, {} };
    store["name"] = UiValue{ UiValue::Kind::String, 0.0, false, "Hero" };
    store["alive"] = UiValue{ UiValue::Kind::Bool, 0.0, true, {} };

    UiValue v;
    CHECK(eval_binding("5", store, v, err));
    CHECK(v.kind == UiValue::Kind::Number && v.number == 5.0);
    CHECK(eval_binding("true", store, v, err));
    CHECK(v.kind == UiValue::Kind::Bool && v.boolean);
    CHECK(eval_binding("\"hi\"", store, v, err));
    CHECK(v.kind == UiValue::Kind::String && v.string == "hi");

    CHECK(eval_binding("$hp", store, v, err));
    CHECK(v.kind == UiValue::Kind::Number && v.number == 5.0);
    CHECK(eval_binding("$name", store, v, err));
    CHECK(v.string == "Hero");
    CHECK(eval_binding("$alive", store, v, err));
    CHECK(v.kind == UiValue::Kind::Bool && v.boolean);

    // Precedence: 2 + 3 * 4 = 14 (not 20).
    CHECK(eval_binding("2 + 3 * 4", store, v, err));
    CHECK(v.number == 14.0);
    // Parentheses override: (2 + 3) * 4 = 20.
    CHECK(eval_binding("(2 + 3) * 4", store, v, err));
    CHECK(v.number == 20.0);
    // Store arithmetic: $hp * 2 - 1 = 9.
    CHECK(eval_binding("$hp * 2 - 1", store, v, err));
    CHECK(v.number == 9.0);

    // Failures.
    CHECK(!eval_binding("$missing", store, v, err));
    CHECK(!err.empty());
    CHECK(!eval_binding("1 + true", store, v, err));
    CHECK(!eval_binding("1 / 0", store, v, err));
    CHECK(!eval_binding("", store, v, err));
    CHECK(!eval_binding("(1", store, v, err));

    // ---- Spec validate ----------------------------------------------------
    UiLayoutSpec spec;
    spec.root = "root";
    spec.tree = make_node("root", 1.0, LayoutDirection::Row);
    spec.tree.children.push_back(make_node("a"));
    spec.tree.children.push_back(make_node("b"));
    CHECK(spec.validate(err));

    // Root not found.
    UiLayoutSpec bad = spec;
    bad.root = "nope";
    CHECK(!bad.validate(err));

    // Duplicate id.
    UiLayoutSpec dup = spec;
    dup.tree.children[0].id = "b";
    CHECK(!dup.validate(err));

    // weight <= 0.
    UiLayoutSpec w0 = spec;
    w0.tree.children[0].weight = 0.0;
    CHECK(!w0.validate(err));

    // max < min.
    UiLayoutSpec mm = spec;
    mm.tree.children[0].min_w = 10.0;
    mm.tree.children[0].max_w = 5.0;
    CHECK(!mm.validate(err));

    // ---- JSON round-trip ---------------------------------------------------
    const std::string json = spec.to_json();
    UiLayoutSpec back;
    CHECK(back.load_from_json(json, err));
    CHECK(back.root == spec.root);
    CHECK(back.tree.id == "root");
    CHECK(back.tree.children.size() == 2);
    CHECK(back.tree.children[0].id == "a");
    CHECK(back.tree.children[1].id == "b");
    // Round-trip is bit-exact.
    CHECK(back.to_json() == json);

    // Malformed JSON refused all-or-nothing.
    UiLayoutSpec keep = back;
    CHECK(!back.load_from_json("{not json", err));
    CHECK(back.to_json() == keep.to_json());

    // ---- layout: row + weights --------------------------------------------
    auto layout = create_ui_layout(spec, err);
    CHECK(layout != nullptr);

    std::vector<LayoutRect> out;
    CHECK(layout->layout(200.0, 100.0, store, out, err));
    // Pre-order: root, a, b.
    CHECK(out.size() == 3);
    CHECK(out[0].id == "root" && near(out[0].w, 200.0) && near(out[0].h, 100.0));
    CHECK(out[1].id == "a");
    CHECK(out[2].id == "b");
    // Row: equal weights split the main (width) axis in half.
    CHECK(near(out[1].w, 100.0) && near(out[1].h, 100.0));
    CHECK(near(out[2].w, 100.0) && near(out[2].h, 100.0));

    // Weight 3:1 split.
    UiLayoutSpec weighted = spec;
    weighted.tree.children[0].weight = 3.0;
    weighted.tree.children[1].weight = 1.0;
    auto wl = create_ui_layout(weighted, err);
    CHECK(wl != nullptr);
    CHECK(wl->layout(200.0, 100.0, store, out, err));
    CHECK(near(out[1].w, 150.0) && near(out[2].w, 50.0));

    // ---- responsive clamp: max_w stops growth -----------------------------
    UiLayoutSpec clamped = spec;
    clamped.tree.children[0].max_w = 25.0;
    auto cl = create_ui_layout(clamped, err);
    CHECK(cl != nullptr);
    CHECK(cl->layout(200.0, 100.0, store, out, err));
    CHECK(near(out[1].w, 25.0)); // clamped
    CHECK(near(out[2].w, 100.0)); // sibling unaffected (100 = 200/2)

    // ---- data binding: visibility hides subtree ---------------------------
    UiLayoutSpec vis = spec;
    vis.tree.children[0].visible_binding = "$alive";
    auto vl = create_ui_layout(vis, err);
    CHECK(vl != nullptr);
    CHECK(vl->layout(200.0, 100.0, store, out, err));
    CHECK(out[1].id == "a" && out[1].visible); // alive=true
    CHECK(out[2].id == "b" && out[2].visible);

    UiStore dead = store;
    dead["alive"] = UiValue{ UiValue::Kind::Bool, 0.0, false, {} };
    CHECK(vl->layout(200.0, 100.0, dead, out, err));
    CHECK(out[1].id == "a" && !out[1].visible); // hidden

    // Hidden child's subtree is also hidden.
    UiLayoutSpec nested = spec;
    nested.tree.children[0].children.push_back(make_node("a1"));
    auto nl = create_ui_layout(nested, err);
    CHECK(nl != nullptr);
    UiLayoutSpec nestedVis = nested;
    nestedVis.tree.children[0].visible_binding = "$alive";
    auto nvl = create_ui_layout(nestedVis, err);
    CHECK(nvl != nullptr);
    CHECK(nvl->layout(200.0, 100.0, dead, out, err));
    bool saw_a1 = false;
    for (const LayoutRect& r : out) {
        if (r.id == "a1") { saw_a1 = true; CHECK(!r.visible); }
    }
    CHECK(!saw_a1); // subtree not recursed into when parent hidden

    // ---- data binding: text ----------------------------------------------
    UiLayoutSpec txt = spec;
    txt.tree.children[0].text_binding = "$name";
    txt.tree.children[1].text_binding = "$hp * 10";
    auto tl = create_ui_layout(txt, err);
    CHECK(tl != nullptr);
    CHECK(tl->layout(200.0, 100.0, store, out, err));
    CHECK(out[1].text == "Hero");
    CHECK(out[2].text == "50"); // 5 * 10

    // ---- binding failure = all-or-nothing (out cleared) -------------------
    UiLayoutSpec broken = spec;
    broken.tree.children[0].text_binding = "$missing";
    auto bl = create_ui_layout(broken, err);
    CHECK(bl != nullptr);
    CHECK(!bl->layout(200.0, 100.0, store, out, err));
    CHECK(out.empty());

    // ---- nested column layout --------------------------------------------
    UiLayoutSpec col = spec;
    col.tree.direction = LayoutDirection::Column;
    auto cl2 = create_ui_layout(col, err);
    CHECK(cl2 != nullptr);
    CHECK(cl2->layout(200.0, 100.0, store, out, err));
    CHECK(near(out[1].w, 200.0) && near(out[1].h, 50.0)); // column splits height
    CHECK(near(out[2].w, 200.0) && near(out[2].h, 50.0));

    // ---- deterministic cross-instance -------------------------------------
    auto l1 = create_ui_layout(spec, err);
    auto l2 = create_ui_layout(spec, err);
    std::vector<LayoutRect> o1, o2;
    CHECK(l1->layout(200.0, 100.0, store, o1, err));
    CHECK(l2->layout(200.0, 100.0, store, o2, err));
    CHECK(o1.size() == o2.size());
    for (std::size_t i = 0; i < o1.size(); ++i) {
        CHECK(o1[i].id == o2[i].id);
        CHECK(o1[i].x == o2[i].x && o1[i].y == o2[i].y);
        CHECK(o1[i].w == o2[i].w && o1[i].h == o2[i].h);
        CHECK(o1[i].text == o2[i].text);
    }

    // ---- invalid container size -------------------------------------------
    CHECK(!l1->layout(-1.0, 100.0, store, out, err));
    CHECK(!err.empty());

    std::cout << "UiLayoutTests: all checks passed\n";
    return true;
}

}  // namespace

int main() {
    if (!run_all()) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
