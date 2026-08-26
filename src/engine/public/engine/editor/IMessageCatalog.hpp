#pragma once

// IMessageCatalog (agente 2 §C): the PUBLIC message-consistency contract —
// ONE data-driven catalog of user-facing messages (editor/UI/system), so a
// message has a STABLE id, a severity, a parameterized text and an ACTIONABLE
// hint (what the user can do about it). Consumed by the shell's status bar,
// toasts, dialogs and error panels — replacing ad-hoc strings with a
// curated, testable, localizable catalog.
//   - STABLE IDS: "asset.import.failed", "save.ok", "command.unknown"…
//     displayed once, referenced everywhere.
//   - PARAMETERIZED TEXT: "{0}".."{n}" placeholders filled at render time.
//     render() always succeeds (never crashes); missing parameters leave the
//     placeholder visible and report a diagnostic.
//   - ACTIONABLE: each message may carry a suggested action string the
//     project interprets (e.g. "asset:open_import_settings"), so errors are
//     actionable, not dead-ends (plano §B "erros acionáveis").
//   - ALL-OR-NOTHING: load_from_json / add never partially apply (empty or
//     duplicate ids refused without mutation).
//   - DETERMINISM: ids() sorted; same doc + params -> identical render,
//     bit-exact. Unknown id: render returns "[unknown:id]" + diagnostic —
//     never a crash, always visible.
//
// Self-contained (std only). The SDK adapter
// (src/engine/sdk/MessageCatalog.cpp) is the ONLY TU with behavior.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace editor {

enum class MessageSeverity : std::uint8_t { Info, Warning, Error };

// One catalog entry.
struct CatalogMessage {
    std::string id;        // stable id (required, unique)
    MessageSeverity severity{ MessageSeverity::Info };
    std::string text;      // template with {0}..{n} placeholders (required)
    std::string action;    // suggested action string ("" = none)

    bool operator==(const CatalogMessage& other) const {
        return id == other.id && severity == other.severity &&
               text == other.text && action == other.action;
    }
    bool operator!=(const CatalogMessage& other) const {
        return !(*this == other);
    }
};

// The full catalog document, versioned and JSON round-trippable.
struct MessageCatalogDoc {
    int version{ 1 };
    std::vector<CatalogMessage> messages;

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    std::string to_json() const;
};

class IMessageCatalog {
public:
    virtual ~IMessageCatalog() = default;

    // Looks up a message by stable id (nullptr when unknown).
    virtual const CatalogMessage* find(const std::string& id) const = 0;

    // Renders a message with parameters filling {0}..{n}. Always succeeds:
    // unknown id -> "[unknown:id]" + diagnostic; missing parameters leave
    // the placeholder visible + diagnostic. Bit-exact for the same inputs.
    virtual std::string render(const std::string& id,
                               const std::vector<std::string>& params,
                               std::string& errorOut) const = 0;

    // All message ids, sorted (deterministic).
    virtual std::vector<std::string> ids() const = 0;

    // Adds a message at runtime (all-or-nothing: empty or duplicate id
    // refused without mutation). Unknown severity ids are refused.
    virtual bool add(const CatalogMessage& message, std::string& errorOut) = 0;

    virtual std::size_t size() const = 0;

    virtual const MessageCatalogDoc& spec() const = 0;
};

// Parses+validates a catalog doc and compiles it (rejected -> nullptr +
// errorOut).
std::unique_ptr<IMessageCatalog> create_message_catalog(
    const MessageCatalogDoc& doc, std::string& errorOut);

}  // namespace editor
}  // namespace engine
