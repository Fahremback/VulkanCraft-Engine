// Gate for engine::editor::IUndoHistory (agente 2 §B) — generic undo/redo
// stack with depth cap and adjacent merge. Headless: no editor, no scene.

#include "engine/editor/IUndoHistory.hpp"

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

// The contract documents "already executed by the caller" (the editor's
// UndoSystem runs execute() before pushing). This helper mirrors that real
// usage: it runs the redo (applies the change) and then pushes.
bool push_executed(engine::editor::IUndoHistory* h, const char* name,
                   int& value, int delta, std::string& err) {
    engine::editor::UndoCommand c;
    c.name = name;
    c.redo = [&value, delta]() { value += delta; };
    c.undo = [&value, delta]() { value -= delta; };
    c.redo();  // the caller applied the change before pushing
    return h->push(std::move(c), err);
}

engine::editor::UndoCommand cmd(const char* name, int& value, int delta) {
    engine::editor::UndoCommand c;
    c.name = name;
    c.redo = [&value, delta]() { value += delta; };
    c.undo = [&value, delta]() { value -= delta; };
    return c;
}

void test_push_undo_redo() {
    int v = 0;
    std::string err;
    auto h = engine::editor::create_undo_history(10);

    CHECK(!h->can_undo() && !h->can_redo(), "fresh history empty");
    CHECK(h->undo_depth() == 0 && h->redo_depth() == 0, "fresh depths zero");
    CHECK(h->top_name().empty(), "fresh top empty");

    CHECK(push_executed(h.get(), "add", v, 5, err), "push 1");
    CHECK(v == 5, "first change applied (caller ran it)");
    CHECK(push_executed(h.get(), "add", v, 3, err), "push 2");
    CHECK(v == 8, "second change applied");
    CHECK(h->undo_depth() == 2, "two undos");
    CHECK(h->top_name() == "add", "top is last pushed");

    CHECK(h->undo(), "undo");
    CHECK(v == 5, "undo reversed last delta");
    CHECK(h->redo_depth() == 1, "redo has one");

    CHECK(h->undo(), "undo again");
    CHECK(v == 0, "both undone");
    CHECK(h->can_redo() && !h->can_undo(), "can redo, cannot undo");

    CHECK(h->redo(), "redo");
    CHECK(v == 5, "redo reapplied first delta");
    CHECK(h->undo_depth() == 1, "redo moved back to undo");

    CHECK(h->redo(), "redo");
    CHECK(v == 8, "redo reapplied second delta");
    CHECK(!h->can_redo(), "redo exhausted");
}

void test_push_invalidates_redo() {
    int v = 0;
    std::string err;
    auto h = engine::editor::create_undo_history(10);

    push_executed(h.get(), "a", v, 1, err);
    push_executed(h.get(), "b", v, 2, err);
    h->undo();  // redo branch: b
    CHECK(h->can_redo(), "redo available");

    push_executed(h.get(), "c", v, 4, err);  // new command clears redo
    CHECK(!h->can_redo(), "new push clears redo branch");
    CHECK(h->undo_depth() == 2, "a + c on undo stack");
}

void test_cap_evicts_oldest() {
    int v = 0;
    std::string err;
    auto h = engine::editor::create_undo_history(3);

    push_executed(h.get(), "1", v, 1, err);
    push_executed(h.get(), "2", v, 2, err);
    push_executed(h.get(), "3", v, 3, err);
    CHECK(v == 6, "all three applied");
    CHECK(h->undo_depth() == 3, "at capacity");

    push_executed(h.get(), "4", v, 4, err);
    CHECK(v == 10, "fourth applied");
    CHECK(h->undo_depth() == 3, "cap enforced");
    CHECK(h->top_name() == "4", "newest kept");
    // Oldest (1) was evicted: undoing 3 times (4,3,2) leaves v = 10-4-3-2
    // = 1 — the evicted command's +1 remains applied forever (it is gone).
    h->undo(); h->undo(); h->undo();
    CHECK(v == 1, "evicted oldest (1) is gone (its +1 remains applied)");
    CHECK(!h->can_undo(), "fully undone");
}

void test_merge_top() {
    int v = 0;
    std::string err;
    auto h = engine::editor::create_undo_history(10);

    push_executed(h.get(), "pos", v, 3, err);
    CHECK(v == 3, "position change applied");
    // Merging a same-name change onto the top swaps the redo, keeps undo.
    // (The caller applies the merged value itself, mirroring the editor.)
    CHECK(h->merge_top("pos", [&v]() { v += 7; }), "merge top same name");
    v += 7;  // caller applies the merged redo
    CHECK(v == 10, "merged value applied by caller");
    CHECK(h->undo_depth() == 1, "merged, not pushed");
    CHECK(h->undo(), "undo merged");
    CHECK(v == 7, "merged undo restores the ORIGINAL undo (3 reversed)");
    CHECK(h->redo(), "redo merged");
    CHECK(v == 14, "merged redo applies the NEW redo (7)");

    // Different name -> no merge (false, no mutation); the caller would then
    // fall back to push() — here we just verify merge itself did not push.
    CHECK(!h->merge_top("other", [&v]() { v += 1; }), "different name no merge");
    CHECK(h->undo_depth() == 1, "merge did not mutate the stack");
}

void test_refusals() {
    int v = 0;
    std::string err;
    auto h = engine::editor::create_undo_history(10);

    engine::editor::UndoCommand empty;
    CHECK(!h->push(empty, err), "empty name refused");
    CHECK(!err.empty(), "refusal carries reason");
    CHECK(h->undo_depth() == 0, "refusal did not mutate");

    CHECK(!h->undo(), "undo on empty is false");
    CHECK(!h->redo(), "redo on empty is false");

    // merge_top on empty / with redo branch / wrong name.
    CHECK(!h->merge_top("x", [&v]() { v += 1; }), "merge on empty false");
    push_executed(h.get(), "a", v, 1, err);
    push_executed(h.get(), "b", v, 2, err);
    h->undo();  // redo branch active
    CHECK(!h->merge_top("b", [&v]() { v += 9; }), "merge blocked by redo branch");
}

void test_clear() {
    int v = 0;
    std::string err;
    auto h = engine::editor::create_undo_history(10);
    push_executed(h.get(), "a", v, 1, err);
    push_executed(h.get(), "b", v, 2, err);
    h->clear();
    CHECK(h->undo_depth() == 0 && h->redo_depth() == 0, "clear empties");
    CHECK(!h->can_undo() && !h->can_redo(), "clear resets flags");
}

void test_determinism() {
    int va = 0, vb = 0;
    std::string err;
    auto a = engine::editor::create_undo_history(5);
    auto b = engine::editor::create_undo_history(5);

    for (int i = 0; i < 6; ++i) {  // 6 pushes on cap 5 -> evicts
        push_executed(a.get(), "n", va, i + 1, err);
        push_executed(b.get(), "n", vb, i + 1, err);
    }
    a->undo(); b->undo();
    a->merge_top("n", [&va]() { va += 100; });
    b->merge_top("n", [&vb]() { vb += 100; });

    CHECK(a->to_json() == b->to_json(), "JSON determinism cross-instance");
    CHECK(a->undo_depth() == b->undo_depth() && a->redo_depth() == b->redo_depth(),
          "depths equal cross-instance");
}

void test_json() {
    int v = 0;
    std::string err;
    auto h = engine::editor::create_undo_history(10);
    push_executed(h.get(), "move", v, 1, err);
    push_executed(h.get(), "scale", v, 2, err);
    const std::string json = h->to_json();
    CHECK(json.find("\"undo_depth\":2") != std::string::npos, "json undo depth");
    CHECK(json.find("\"redo_depth\":0") != std::string::npos, "json redo depth");
    CHECK(json.find("\"can_undo\":true") != std::string::npos, "json can_undo");
    CHECK(json.find("\"top\":\"scale\"") != std::string::npos, "json top");
}

}  // namespace

int main() {
    test_push_undo_redo();
    test_push_invalidates_redo();
    test_cap_evicts_oldest();
    test_merge_top();
    test_refusals();
    test_clear();
    test_determinism();
    test_json();

    if (g_failures == 0) {
        std::printf("ALL PASSED\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
