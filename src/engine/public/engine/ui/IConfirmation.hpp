#pragma once

// IConfirmation (agente 2 §A item 5): the PUBLIC action-confirmation
// contract — actions that must be confirmed by the user AND, when required,
// authorized by the server before they execute. ILayout (#177) binds data,
// IWidgets (#183) renders modals; this contract is the DECISION layer under
// those modals: a single pending action goes through a deterministic state
// machine (None -> Pending -> Confirmed | Cancelled | Rejected), and the
// project executes the action STRING only after confirmation and after the
// (optional) authority gate approves.
//   - REQUEST: sets the pending action (all-or-nothing: refuses while a
//     different action is already pending; a resolved action may be replaced).
//   - CONFIRM: with no authority gate installed the action is confirmed
//     immediately (user confirmation IS the authority). With a gate, the gate
//     runs first: refusal transitions to Rejected and NEVER executes.
//   - CANCEL: transitions to Cancelled without executing.
//   - RESULT: {state, action, reject_reason} — the project interprets
//     `action` (an opaque action string from the spec, e.g. "server:buy:42")
//     and owns the actual mutation, keeping gameplay logic out of this layer.
//
// Deterministic and headless: same spec + gate + call sequence -> identical
// states, bit-exact. The authority gate is an injected std::function, so
// tests exercise server-refusal without a network. Self-contained
// (std + engine/ui only). The SDK adapter (src/engine/sdk/UiConfirmation.cpp)
// is the ONLY TU with behavior.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace engine {
namespace ui {

enum class ConfirmSeverity : std::uint8_t { Info, Warning, Danger };

enum class ConfirmState : std::uint8_t {
    None,       // nothing requested yet (or after reset)
    Pending,    // awaiting user decision
    Confirmed,  // user confirmed (and authority approved) — action ready
    Cancelled,  // user cancelled — on_cancel action ready
    Rejected,   // authority gate refused — nothing executes
};

struct ConfirmActionSpec {
    std::string id;            // unique action id (required)
    std::string title;         // short title (required)
    std::string message;       // body text (optional)
    ConfirmSeverity severity{ ConfirmSeverity::Info };
    std::string payload;       // opaque JSON payload (optional)
    std::string confirm_label{ "Confirm" };
    std::string cancel_label{ "Cancel" };
    std::string on_confirm;    // action string executed on confirm
    std::string on_cancel;     // action string executed on cancel

    bool operator==(const ConfirmActionSpec& other) const {
        return id == other.id && title == other.title &&
               message == other.message && severity == other.severity &&
               payload == other.payload &&
               confirm_label == other.confirm_label &&
               cancel_label == other.cancel_label &&
               on_confirm == other.on_confirm && on_cancel == other.on_cancel;
    }
    bool operator!=(const ConfirmActionSpec& other) const {
        return !(*this == other);
    }

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    std::string to_json() const;
};

struct ConfirmResult {
    ConfirmState state{ ConfirmState::None };
    std::string action;        // on_confirm / on_cancel when resolved
    std::string reject_reason; // set when state == Rejected
};

class IUiConfirmation {
public:
    virtual ~IUiConfirmation() = default;

    // Sets the pending action. All-or-nothing: refuses when a different
    // action is already pending; a resolved action (Confirmed/Cancelled/
    // Rejected) may be replaced. Returns false + errorOut on refusal without
    // mutating state.
    virtual bool request(const ConfirmActionSpec& spec,
                         std::string& errorOut) = 0;

    // True while an action awaits the user's decision.
    virtual bool is_pending() const = 0;

    // The current pending action (empty when none).
    virtual ConfirmActionSpec pending_spec() const = 0;

    // Confirms the pending action. Runs the authority gate (if installed)
    // FIRST: a refusal transitions to Rejected and the action NEVER executes.
    // Refuses (no mutation) when nothing is pending.
    virtual bool confirm(std::string& errorOut) = 0;

    // Cancels the pending action (on_cancel action ready). Refuses (no
    // mutation) when nothing is pending.
    virtual bool cancel(std::string& errorOut) = 0;

    // Resets to None, discarding any pending/resolved action.
    virtual void reset() = 0;

    // Latest decision: state + the action string the project must execute
    // (empty while Pending or None).
    virtual ConfirmResult result() const = 0;

    // Installs the server-authority gate: called on confirm(); returning
    // false with a reason transitions to Rejected. Absent gate == user
    // confirmation is sufficient authority.
    virtual void set_authority_check(
        std::function<bool(const ConfirmActionSpec&, std::string&)>
            check) = 0;

    virtual const ConfirmActionSpec& spec() const = 0;
};

// Creates an empty confirmation runtime (no pending action). Always
// succeeds; authority gate is optional.
std::unique_ptr<IUiConfirmation> create_ui_confirmation(std::string& errorOut);

}  // namespace ui
}  // namespace engine
