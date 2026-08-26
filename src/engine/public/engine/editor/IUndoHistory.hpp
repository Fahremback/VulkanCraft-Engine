#pragma once

// IUndoHistory (agente 2 §B): the PUBLIC undo/redo stack contract. The
// editor's UndoSystem is Scene-bound and has NO depth cap (the undo stack
// grows unbounded — a memory leak on long sessions) and NO observability.
// This contract provides the GENERIC stack mechanics (depth cap, adjacent
// merge, deterministic JSON) that the editor's UndoSystem delegates to:
//   - CAP: push beyond capacity evicts the OLDEST undo entry (never the
//     newest); redo entries are not counted against the cap.
//   - MERGE: merge_top(name, redoFn) collapses a new "same property changed
//     again" onto the top entry (keeps the original undo, swaps the redo) —
//     only when the top entry has the same name and the redo stack is empty
//     (standard property-drag coalescing).
//   - ALL-OR-NOTHING: push/merge refuse empty names without mutating.
//   - DETERMINISM: pure data structure, no clocks/RNG/globals; same command
//     sequence -> identical state and JSON, bit-exact.
//   - OBSERVABLE: to_json() serializes {undo_depth, redo_depth, can_undo,
//     can_redo, top} deterministically (editor exposes it via the Control
//     API, e.g. GET /undo).
//
// Self-contained (std only). The SDK adapter
// (src/engine/sdk/UndoHistory.cpp) is the ONLY TU with behavior.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace engine {
namespace editor {

// One reversible command (the editor wraps its Scene-bound commands into
// these closures when delegating to the contract).
struct UndoCommand {
    std::string name;                 // display/merge key (required, unique-ish)
    std::function<void()> undo;       // called by undo()
    std::function<void()> redo;       // called by redo() / merge_top swap
};

class IUndoHistory {
public:
    virtual ~IUndoHistory() = default;

    // Pushes a command (already executed by the caller). Empty name refused.
    // Beyond capacity: the OLDEST undo entry is evicted (its closures die).
    virtual bool push(UndoCommand command, std::string& errorOut) = 0;

    // If the top undo entry has the same name and the redo stack is empty,
    // replaces its redo closure with `redoFn` (keeps the original undo) and
    // returns true. Otherwise returns false WITHOUT mutating — the caller
    // falls back to push() (mirrors the editor's property-merge: merge on
    // success, push on failure).
    virtual bool merge_top(const std::string& name,
                           std::function<void()> redoFn) = 0;

    // Undoes the top entry (moves it to the redo stack). False when empty.
    virtual bool undo() = 0;

    // Redoes the top redo entry (moves it back to the undo stack). False when
    // empty.
    virtual bool redo() = 0;

    // Clears both stacks (all closures die).
    virtual void clear() = 0;

    virtual bool can_undo() const = 0;
    virtual bool can_redo() const = 0;

    virtual std::size_t undo_depth() const = 0;
    virtual std::size_t redo_depth() const = 0;

    // Name of the top undo entry ("" when empty).
    virtual std::string top_name() const = 0;

    // Deterministic JSON snapshot ({"undo_depth":N,"redo_depth":M,...}).
    virtual std::string to_json() const = 0;
};

// Creates the history with a max undo depth. depth == 0 -> default 256.
// Never returns nullptr.
std::unique_ptr<IUndoHistory> create_undo_history(std::size_t depth);

}  // namespace editor
}  // namespace engine
