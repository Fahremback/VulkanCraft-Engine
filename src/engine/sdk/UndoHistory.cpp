// UndoHistory.cpp — the ONLY TU with the undo-history behavior (agente 2 §B).
// Generic undo/redo stack: depth cap (evicts oldest), adjacent merge for
// property coalescing, deterministic JSON snapshot. No clocks/RNG/globals.

#include "engine/editor/IUndoHistory.hpp"

#include <deque>
#include <sstream>

namespace engine {
namespace editor {

namespace {

class UndoHistoryImpl final : public IUndoHistory {
public:
    explicit UndoHistoryImpl(std::size_t depth)
        : depth_(depth == 0 ? 256 : depth) {}

    bool push(UndoCommand command, std::string& errorOut) override {
        errorOut.clear();
        if (command.name.empty()) {
            errorOut = "undo command name must not be empty";
            return false;
        }
        undo_.push_back(std::move(command));
        if (undo_.size() > depth_) {
            undo_.pop_front();  // evict the OLDEST entry (never the newest)
        }
        redo_.clear();  // a new command invalidates the redo branch
        return true;
    }

    bool merge_top(const std::string& name,
                   std::function<void()> redoFn) override {
        if (name.empty()) return false;
        if (undo_.empty() || !redo_.empty()) return false;
        if (undo_.back().name != name) return false;
        undo_.back().redo = std::move(redoFn);
        return true;
    }

    bool undo() override {
        if (undo_.empty()) return false;
        UndoCommand cmd = std::move(undo_.back());
        undo_.pop_back();
        if (cmd.undo) cmd.undo();
        redo_.push_back(std::move(cmd));
        return true;
    }

    bool redo() override {
        if (redo_.empty()) return false;
        UndoCommand cmd = std::move(redo_.back());
        redo_.pop_back();
        if (cmd.redo) cmd.redo();
        undo_.push_back(std::move(cmd));
        return true;
    }

    void clear() override {
        undo_.clear();
        redo_.clear();
    }

    bool can_undo() const override { return !undo_.empty(); }
    bool can_redo() const override { return !redo_.empty(); }

    std::size_t undo_depth() const override { return undo_.size(); }
    std::size_t redo_depth() const override { return redo_.size(); }

    std::string top_name() const override {
        return undo_.empty() ? std::string() : undo_.back().name;
    }

    std::string to_json() const override {
        std::ostringstream out;
        out << "{\"undo_depth\":" << undo_.size()
            << ",\"redo_depth\":" << redo_.size()
            << ",\"can_undo\":" << (undo_.empty() ? "false" : "true")
            << ",\"can_redo\":" << (redo_.empty() ? "false" : "true")
            << ",\"top\":\"" << top_name() << "\"}";
        return out.str();
    }

private:
    std::size_t depth_;
    std::deque<UndoCommand> undo_;
    std::deque<UndoCommand> redo_;
};

}  // namespace

std::unique_ptr<IUndoHistory> create_undo_history(std::size_t depth) {
    return std::make_unique<UndoHistoryImpl>(depth);
}

}  // namespace editor
}  // namespace engine
