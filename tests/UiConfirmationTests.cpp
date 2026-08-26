// UiConfirmationTests — headless coverage for the public action-confirmation
// contract (engine/ui/IConfirmation.hpp, adapter UiConfirmation.cpp): the
// deterministic state machine (None -> Pending -> Confirmed | Cancelled |
// Rejected), the optional server-authority gate (refusal never executes),
// all-or-nothing request rules, JSON bit-exact round-trip, and cross-instance
// determinism. Standalone main() with CHECK (pattern: UiWidgetsTests).

#include "engine/ui/IConfirmation.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace engine::ui;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "UiConfirmationTests failure at line " << __LINE__ << ": " #condition "\n"; \
    return false; } } while (false)

namespace {

ConfirmActionSpec make_spec(const std::string& id = "buy_sword",
                            const std::string& onConfirm = "server:buy:42") {
    ConfirmActionSpec spec;
    spec.id = id;
    spec.title = "Buy sword";
    spec.message = "This costs 10 gold.";
    spec.severity = ConfirmSeverity::Danger;
    spec.payload = "{\"item\":\"sword\",\"price\":10}";
    spec.confirm_label = "Buy";
    spec.cancel_label = "Not now";
    spec.on_confirm = onConfirm;
    spec.on_cancel = "server:dismiss:42";
    return spec;
}

bool run_all() {
    std::string err;

    // ---- Initial state ---------------------------------------------------
    {
        auto c = create_ui_confirmation(err);
        CHECK(c != nullptr);
        CHECK(c->result().state == ConfirmState::None);
        CHECK(!c->is_pending());
        CHECK(c->pending_spec().id.empty());
    }

    // ---- Confirm without authority -> Confirmed + action ----------------
    {
        auto c = create_ui_confirmation(err);
        CHECK(c->request(make_spec(), err));
        CHECK(c->is_pending());
        CHECK(c->confirm(err));
        const ConfirmResult r = c->result();
        CHECK(r.state == ConfirmState::Confirmed);
        CHECK(r.action == "server:buy:42");
        CHECK(r.reject_reason.empty());
        CHECK(!c->is_pending());
    }

    // ---- Cancel -> Cancelled + on_cancel action -------------------------
    {
        auto c = create_ui_confirmation(err);
        CHECK(c->request(make_spec(), err));
        CHECK(c->cancel(err));
        const ConfirmResult r = c->result();
        CHECK(r.state == ConfirmState::Cancelled);
        CHECK(r.action == "server:dismiss:42");
    }

    // ---- Authority gate refuses -> Rejected, action NEVER executes ------
    {
        auto c = create_ui_confirmation(err);
        c->set_authority_check([](const ConfirmActionSpec&, std::string& reason) {
            reason = "not enough gold";
            return false;
        });
        CHECK(c->request(make_spec(), err));
        CHECK(c->confirm(err));
        const ConfirmResult r = c->result();
        CHECK(r.state == ConfirmState::Rejected);
        CHECK(r.action.empty());            // nothing to execute
        CHECK(r.reject_reason == "not enough gold");
    }

    // ---- Authority gate approves -> Confirmed ---------------------------
    {
        auto c = create_ui_confirmation(err);
        int approvals = 0;
        c->set_authority_check([&](const ConfirmActionSpec& s, std::string&) {
            CHECK(s.id == "buy_sword");     // gate sees the pending action
            ++approvals;
            return true;
        });
        CHECK(c->request(make_spec(), err));
        CHECK(c->confirm(err));
        CHECK(c->result().state == ConfirmState::Confirmed);
        CHECK(approvals == 1);
    }

    // ---- Confirm/cancel with nothing pending -> refusal, no mutation ----
    {
        auto c = create_ui_confirmation(err);
        CHECK(!c->confirm(err));            // refused
        CHECK(!err.empty());
        CHECK(c->result().state == ConfirmState::None);
        err.clear();
        CHECK(!c->cancel(err));             // refused
        CHECK(c->result().state == ConfirmState::None);
    }

    // ---- Double request while pending -> refusal, first kept ------------
    {
        auto c = create_ui_confirmation(err);
        CHECK(c->request(make_spec("buy_sword"), err));
        CHECK(!c->request(make_spec("sell_sword"), err));  // different id
        CHECK(c->pending_spec().id == "buy_sword");
        CHECK(c->result().state == ConfirmState::Pending);
    }

    // ---- Request after resolved replaces ---------------------------------
    {
        auto c = create_ui_confirmation(err);
        CHECK(c->request(make_spec("buy_sword"), err));
        CHECK(c->confirm(err));
        CHECK(c->result().state == ConfirmState::Confirmed);
        CHECK(c->request(make_spec("sell_sword"), err));  // replaces
        CHECK(c->is_pending());
        CHECK(c->pending_spec().id == "sell_sword");
    }

    // ---- Reset ------------------------------------------------------------
    {
        auto c = create_ui_confirmation(err);
        CHECK(c->request(make_spec(), err));
        CHECK(c->confirm(err));
        c->reset();
        CHECK(c->result().state == ConfirmState::None);
        CHECK(!c->is_pending());
        CHECK(!c->confirm(err));            // nothing pending again
    }

    // ---- Spec validate + JSON round-trip + refusals ----------------------
    {
        const ConfirmActionSpec spec = make_spec();
        CHECK(spec.validate(err));
        const std::string jsonText = spec.to_json();

        ConfirmActionSpec back;
        CHECK(back.load_from_json(jsonText, err));
        CHECK(back == spec);                // bit-exact round-trip

        // Invalid JSON does not mutate.
        ConfirmActionSpec untouched = make_spec("keep");
        CHECK(!untouched.load_from_json("{not json", err));
        CHECK(untouched.id == "keep");

        // Unsupported version does not mutate.
        CHECK(!untouched.load_from_json("{\"version\":99,\"id\":\"x\",\"title\":\"t\"}", err));
        CHECK(untouched.id == "keep");

        // Unknown severity does not mutate.
        CHECK(!untouched.load_from_json(
            "{\"version\":1,\"id\":\"x\",\"title\":\"t\",\"severity\":\"epic\"}", err));
        CHECK(untouched.id == "keep");

        // Missing id/title -> invalid.
        ConfirmActionSpec empty;
        CHECK(!empty.validate(err));
        empty.id = "x";
        CHECK(!empty.validate(err));
    }

    // ---- Determinism cross-instance --------------------------------------
    {
        const ConfirmActionSpec spec = make_spec();
        for (int i = 0; i < 2; ++i) {
            auto c = create_ui_confirmation(err);
            CHECK(c->request(spec, err));
            CHECK(c->confirm(err));
            CHECK(c->result().state == ConfirmState::Confirmed);
            CHECK(c->result().action == "server:buy:42");
        }
        for (int i = 0; i < 2; ++i) {
            auto c = create_ui_confirmation(err);
            c->set_authority_check([](const ConfirmActionSpec&, std::string& reason) {
                reason = "no";
                return false;
            });
            CHECK(c->request(spec, err));
            CHECK(c->confirm(err));
            CHECK(c->result().state == ConfirmState::Rejected);
            CHECK(c->result().reject_reason == "no");
        }
    }

    std::cout << "UiConfirmationTests: all checks passed\n";
    return true;
}

}  // namespace

int main() {
    if (!run_all()) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
