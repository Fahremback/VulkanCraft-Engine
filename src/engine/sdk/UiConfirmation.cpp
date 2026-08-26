// UiConfirmation.cpp — the ONLY TU with the action-confirmation runtime
// (agente 2 §A item 5). Pure and deterministic: a single pending action
// walks None -> Pending -> Confirmed | Cancelled | Rejected. The authority
// gate is an injected std::function run on confirm() BEFORE the action
// executes; a refusal transitions to Rejected and nothing runs. No
// window/GPU/network. JSON parse/emit uses the shared RegistryJson helpers.

#include "engine/ui/IConfirmation.hpp"

#include "RegistryJson.hpp"

#include <sstream>

namespace engine {
namespace ui {

namespace {

const char* severity_name(ConfirmSeverity severity) {
    switch (severity) {
        case ConfirmSeverity::Info: return "info";
        case ConfirmSeverity::Warning: return "warning";
        case ConfirmSeverity::Danger: break;
    }
    return "danger";
}

bool severity_from_name(const std::string& name, ConfirmSeverity& out) {
    if (name == "info") { out = ConfirmSeverity::Info; return true; }
    if (name == "warning") { out = ConfirmSeverity::Warning; return true; }
    if (name == "danger") { out = ConfirmSeverity::Danger; return true; }
    return false;
}

// Escapes a string for embedding in JSON (the shared RegistryJson parser
// understands \\ \" \n \t \r etc.; control chars are rejected raw).
std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

}  // namespace

bool ConfirmActionSpec::validate(std::string& errorOut) const {
    errorOut.clear();
    if (id.empty()) {
        errorOut = "confirmation action id must not be empty";
        return false;
    }
    if (title.empty()) {
        errorOut = "confirmation action title must not be empty";
        return false;
    }
    return true;
}

std::string ConfirmActionSpec::to_json() const {
    std::ostringstream out;
    out.precision(9);
    out << "{\"id\":\"" << json_escape(id) << "\",\"title\":\""
        << json_escape(title) << "\",\"message\":\""
        << json_escape(message) << "\",\"severity\":\""
        << severity_name(severity) << "\",\"payload\":\""
        << json_escape(payload) << "\",\"confirm_label\":\""
        << json_escape(confirm_label) << "\",\"cancel_label\":\""
        << json_escape(cancel_label) << "\",\"on_confirm\":\""
        << json_escape(on_confirm) << "\",\"on_cancel\":\""
        << json_escape(on_cancel) << "\"}";
    return out.str();
}

bool ConfirmActionSpec::load_from_json(const std::string& jsonText,
                                       std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(jsonText, doc, errorOut)) return false;
    if (!doc.is_object()) {
        errorOut = "confirmation action document must be an object";
        return false;
    }
    const int version = static_cast<int>(sdk::json_number(doc, "version", 1));
    if (version != 1) {
        errorOut = "unsupported confirmation action version";
        return false;
    }
    ConfirmActionSpec candidate;
    candidate.id = sdk::json_string(doc, "id", "");
    candidate.title = sdk::json_string(doc, "title", "");
    candidate.message = sdk::json_string(doc, "message", "");
    const std::string severity = sdk::json_string(doc, "severity", "info");
    if (!severity_from_name(severity, candidate.severity)) {
        errorOut = "unknown confirmation severity: " + severity;
        return false;
    }
    candidate.payload = sdk::json_string(doc, "payload", "");
    candidate.confirm_label = sdk::json_string(doc, "confirm_label", "Confirm");
    candidate.cancel_label = sdk::json_string(doc, "cancel_label", "Cancel");
    candidate.on_confirm = sdk::json_string(doc, "on_confirm", "");
    candidate.on_cancel = sdk::json_string(doc, "on_cancel", "");
    if (!candidate.validate(errorOut)) return false;
    *this = std::move(candidate);
    return true;
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

namespace {

class UiConfirmationRuntime final : public IUiConfirmation {
public:
    UiConfirmationRuntime() = default;

    bool request(const ConfirmActionSpec& spec, std::string& errorOut) override {
        errorOut.clear();
        if (state_ == ConfirmState::Pending && pending_.id != spec.id) {
            errorOut = "another confirmation action is already pending: " +
                       pending_.id;
            return false;
        }
        pending_ = spec;
        state_ = ConfirmState::Pending;
        result_ = ConfirmResult{};
        result_.state = ConfirmState::Pending;
        return true;
    }

    bool is_pending() const override {
        return state_ == ConfirmState::Pending;
    }

    ConfirmActionSpec pending_spec() const override { return pending_; }

    bool confirm(std::string& errorOut) override {
        errorOut.clear();
        if (state_ != ConfirmState::Pending) {
            errorOut = "no confirmation action is pending";
            return false;
        }
        if (authority_) {
            std::string reason;
            if (!authority_(pending_, reason)) {
                state_ = ConfirmState::Rejected;
                result_ = ConfirmResult{};
                result_.state = ConfirmState::Rejected;
                result_.reject_reason = reason;
                return true;
            }
        }
        state_ = ConfirmState::Confirmed;
        result_ = ConfirmResult{};
        result_.state = ConfirmState::Confirmed;
        result_.action = pending_.on_confirm;
        return true;
    }

    bool cancel(std::string& errorOut) override {
        errorOut.clear();
        if (state_ != ConfirmState::Pending) {
            errorOut = "no confirmation action is pending";
            return false;
        }
        state_ = ConfirmState::Cancelled;
        result_ = ConfirmResult{};
        result_.state = ConfirmState::Cancelled;
        result_.action = pending_.on_cancel;
        return true;
    }

    void reset() override {
        pending_ = ConfirmActionSpec{};
        state_ = ConfirmState::None;
        result_ = ConfirmResult{};
    }

    ConfirmResult result() const override { return result_; }

    void set_authority_check(
        std::function<bool(const ConfirmActionSpec&, std::string&)> check)
        override {
        authority_ = std::move(check);
    }

    const ConfirmActionSpec& spec() const override { return pending_; }

private:
    ConfirmActionSpec pending_;
    ConfirmState state_{ ConfirmState::None };
    ConfirmResult result_;
    std::function<bool(const ConfirmActionSpec&, std::string&)> authority_;
};

}  // namespace

std::unique_ptr<IUiConfirmation> create_ui_confirmation(std::string& errorOut) {
    errorOut.clear();
    return std::make_unique<UiConfirmationRuntime>();
}

}  // namespace ui
}  // namespace engine
