// UiDoc.cpp — the ONLY TU with the UI-composition behavior (agente 2 §A
// item 6). Composes the four engine/ui contracts (layout, widgets, viewport,
// confirmations) into ONE versioned JSON document. load_from_json extracts
// each sub-document and DELEGATES to the sub-contract's own all-or-nothing
// loader; the shared RegistryJson parser has no emit path, so a small local
// emitter (json_emit) re-serializes the extracted fields (std::map object
// iteration is sorted -> deterministic). No window/GPU.

#include "engine/ui/IUiDoc.hpp"

#include "RegistryJson.hpp"

#include <cmath>
#include <sstream>

namespace engine {
namespace ui {

namespace {

// Escapes a string for JSON embedding (mirror of UiConfirmation.cpp).
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

// Re-emits a parsed JsonValue back to JSON text (deterministic: object keys
// iterate in std::map sorted order). Numbers use %.9g to match the
// sub-contracts' bit-exact formatting.
void json_emit(const sdk::JsonValue& value, std::ostringstream& out) {
    switch (value.kind) {
        case sdk::JsonValue::Kind::Null:
            out << "null";
            break;
        case sdk::JsonValue::Kind::Bool:
            out << (value.boolean ? "true" : "false");
            break;
        case sdk::JsonValue::Kind::Number: {
            std::ostringstream num;
            num.precision(9);
            num << value.number;
            out << num.str();
            break;
        }
        case sdk::JsonValue::Kind::String:
            out << '"' << json_escape(value.string) << '"';
            break;
        case sdk::JsonValue::Kind::Array: {
            out << '[';
            for (std::size_t i = 0; i < value.array.size(); ++i) {
                if (i != 0) out << ',';
                json_emit(value.array[i], out);
            }
            out << ']';
            break;
        }
        case sdk::JsonValue::Kind::Object: {
            out << '{';
            std::size_t i = 0;
            for (const auto& entry : value.object) {
                if (i++ != 0) out << ',';
                out << '"' << json_escape(entry.first) << "\":";
                json_emit(entry.second, out);
            }
            out << '}';
            break;
        }
    }
}

// Extracts a required object field and re-emits it as JSON text.
bool extract_object(const sdk::JsonValue& doc, const std::string& key,
                    std::string& outText, std::string& errorOut) {
    const sdk::JsonValue* field = doc.field(key);
    if (field == nullptr || !field->is_object()) {
        errorOut = "ui doc is missing required object field: " + key;
        return false;
    }
    std::ostringstream out;
    json_emit(*field, out);
    outText = out.str();
    return true;
}

}  // namespace

bool UiDoc::validate(std::string& errorOut) const {
    errorOut.clear();
    if (version != 1) {
        errorOut = "unsupported ui doc version";
        return false;
    }
    if (!layout.validate(errorOut)) return false;
    if (!widgets.validate(errorOut)) return false;
    if (!viewport.validate(errorOut)) return false;
    for (const ConfirmActionSpec& action : confirmations) {
        if (!action.validate(errorOut)) return false;
    }
    return true;
}

std::string UiDoc::to_json() const {
    std::ostringstream out;
    out.precision(9);
    out << "{\"version\":" << version << ",\"layout\":"
        << layout.to_json() << ",\"widgets\":"
        << widgets.to_json() << ",\"viewport\":"
        << viewport.to_json() << ",\"confirmations\":[";
    for (std::size_t i = 0; i < confirmations.size(); ++i) {
        if (i != 0) out << ',';
        out << confirmations[i].to_json();
    }
    out << "]}";
    return out.str();
}

bool UiDoc::load_from_json(const std::string& jsonText,
                           std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(jsonText, doc, errorOut)) return false;
    if (!doc.is_object()) {
        errorOut = "ui doc document must be an object";
        return false;
    }
    const int version = static_cast<int>(sdk::json_number(doc, "version", 1));
    if (version != 1) {
        errorOut = "unsupported ui doc version";
        return false;
    }

    // Build a candidate; only commit at the end (all-or-nothing).
    UiDoc candidate;
    candidate.version = version;

    std::string fieldText;
    if (!extract_object(doc, "layout", fieldText, errorOut)) return false;
    if (!candidate.layout.load_from_json(fieldText, errorOut)) return false;

    if (!extract_object(doc, "widgets", fieldText, errorOut)) return false;
    if (!candidate.widgets.load_from_json(fieldText, errorOut)) return false;

    if (!extract_object(doc, "viewport", fieldText, errorOut)) return false;
    if (!candidate.viewport.load_from_json(fieldText, errorOut)) return false;

    if (const sdk::JsonValue* actions = doc.field("confirmations")) {
        if (!actions->is_array()) {
            errorOut = "ui doc field 'confirmations' must be an array";
            return false;
        }
        for (const sdk::JsonValue& entry : actions->array) {
            std::ostringstream out;
            json_emit(entry, out);
            ConfirmActionSpec action;
            if (!action.load_from_json(out.str(), errorOut)) return false;
            candidate.confirmations.push_back(std::move(action));
        }
    }

    *this = std::move(candidate);
    return true;
}

}  // namespace ui
}  // namespace engine
