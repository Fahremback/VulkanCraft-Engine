#pragma once

// ICommandSearch (agente 2 §B): the PUBLIC backbone of the editor's global
// search / command palette (Ctrl+K). Today the palette is a hardcoded vector
// of labels + a substring filter with NO deterministic ranking — this
// contract makes the search data-driven and bit-exact testable:
//   - COMMAND INDEX: entries {id, label, category, keywords[], action}
//     versioned JSON round-trippable (all-or-nothing on load).
//   - RANKED SEARCH: query -> deterministic score ordering. Ties broken by
//     (category, id) so the result list is bit-exact for the same index +
//     query. Empty query -> full index in declaration order.
//   - SCORING (documented, deterministic):
//       prefix match on label        : +100
//       word-boundary match on label : +60
//       substring match on label     : +30
//       any keyword prefix/substring : +20
//       category prefix match        : +10
//     Longest-prefix-wins: among equal scores, entries whose label starts
//     with a LONGER prefix of the query rank first (stable secondary sort).
//   - ALL-OR-NOTHING: load_from_json never partially applies (empty/duplicate
//     id refused without mutation).
//   - CONSUMER: the shell palette renders search(query) results and executes
//     entry.action on Enter — the index is the single source of truth.
//
// Self-contained (std only). The SDK adapter
// (src/engine/sdk/CommandSearch.cpp) is the ONLY TU with behavior.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace editor {

// One palette command.
struct CommandEntry {
    std::string id;           // stable id (required, unique)
    std::string label;        // display label (required, e.g. "Save Scene")
    std::string category;     // group ("" = uncategorized)
    std::vector<std::string> keywords;  // extra searchable terms
    std::string action;       // what the editor runs on Enter ("" = none)

    bool operator==(const CommandEntry& other) const {
        return id == other.id && label == other.label &&
               category == other.category && keywords == other.keywords &&
               action == other.action;
    }
    bool operator!=(const CommandEntry& other) const {
        return !(*this == other);
    }
};

// The command index document, versioned and JSON round-trippable.
struct CommandIndexDoc {
    int version{ 1 };
    std::vector<CommandEntry> entries;

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    std::string to_json() const;
};

// One search hit.
struct CommandHit {
    std::string id;
    std::string label;
    std::string category;
    std::string action;
    std::int64_t score{ 0 };  // deterministic ranking score

    bool operator==(const CommandHit& other) const {
        return id == other.id && label == other.label &&
               category == other.category && action == other.action &&
               score == other.score;
    }
    bool operator!=(const CommandHit& other) const {
        return !(*this == other);
    }
};

class ICommandSearch {
public:
    virtual ~ICommandSearch() = default;

    // Ranked search (case-insensitive). Empty query returns the full index in
    // declaration order. Deterministic: same index + query -> identical hits.
    virtual std::vector<CommandHit> search(const std::string& query) const = 0;

    // Looks up one entry by id (nullptr when unknown).
    virtual const CommandEntry* find(const std::string& id) const = 0;

    // Adds an entry at runtime (all-or-nothing: empty/duplicate id refused
    // without mutation).
    virtual bool add(const CommandEntry& entry, std::string& errorOut) = 0;

    virtual std::size_t size() const = 0;

    virtual const CommandIndexDoc& spec() const = 0;
};

// Parses+validates a command index and compiles it (rejected -> nullptr +
// errorOut).
std::unique_ptr<ICommandSearch> create_command_search(
    const CommandIndexDoc& doc, std::string& errorOut);

}  // namespace editor
}  // namespace engine
