// ShortcutDoc.cpp — the ONLY TU with the shortcut-documentation behavior
// (agente 2 §C). Pure and deterministic: humanizes InputBindings and renders
// the CURRENT bindings of an ActionMapSpec as markdown, driven by optional
// display metadata (labels/descriptions). No window/GPU/device. JSON via
// RegistryJson.

#include "engine/editor/IShortcutDoc.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <map>
#include <sstream>

namespace engine {
namespace editor {

namespace {

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

bool ShortcutDocSpec::validate(std::string& errorOut) const {
    errorOut.clear();
    if (version != 1) {
        errorOut = "unsupported shortcut doc version";
        return false;
    }
    std::map<std::string, bool> seen;
    for (const ShortcutEntry& entry : entries) {
        if (entry.action.empty()) {
            errorOut = "shortcut entry action must not be empty";
            return false;
        }
        if (seen.count(entry.action)) {
            errorOut = "duplicate shortcut entry action: " + entry.action;
            return false;
        }
        seen[entry.action] = true;
    }
    return true;
}

std::string ShortcutDocSpec::to_json() const {
    std::ostringstream out;
    out << "{\"version\":" << version << ",\"title\":\""
        << json_escape(title) << "\",\"entries\":[";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i) out << ',';
        out << "{\"action\":\"" << json_escape(entries[i].action)
            << "\",\"label\":\"" << json_escape(entries[i].label)
            << "\",\"description\":\"" << json_escape(entries[i].description)
            << "\"}";
    }
    out << "]}";
    return out.str();
}

bool ShortcutDocSpec::load_from_json(const std::string& jsonText,
                                     std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(jsonText, doc, errorOut)) return false;
    if (!doc.is_object()) {
        errorOut = "shortcut doc document must be an object";
        return false;
    }
    const int version = static_cast<int>(sdk::json_number(doc, "version", 1));
    if (version != 1) {
        errorOut = "unsupported shortcut doc version";
        return false;
    }
    ShortcutDocSpec candidate;
    candidate.version = version;
    candidate.title = sdk::json_string(doc, "title", "");
    if (const sdk::JsonValue* arr = doc.field("entries")) {
        if (!arr->is_array()) {
            errorOut = "shortcut doc field 'entries' must be an array";
            return false;
        }
        for (const sdk::JsonValue& entry : arr->array) {
            ShortcutEntry e;
            e.action = sdk::json_string(entry, "action", "");
            e.label = sdk::json_string(entry, "label", "");
            e.description = sdk::json_string(entry, "description", "");
            candidate.entries.push_back(std::move(e));
        }
    }
    if (!candidate.validate(errorOut)) return false;
    *this = std::move(candidate);
    return true;
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

namespace {

std::string source_name(engine::input::InputSource source) {
    switch (source) {
        case engine::input::InputSource::Keyboard: return "Keyboard";
        case engine::input::InputSource::Mouse: return "Mouse";
        case engine::input::InputSource::Gamepad: return "Gamepad";
        case engine::input::InputSource::Touch: return "Touch";
        case engine::input::InputSource::Other: break;
    }
    return "Other";
}

// Maps a physical input name to a readable label. Falls back to the raw name.
std::string readable_input(const engine::input::InputBinding& binding) {
    using engine::input::InputSource;
    const std::string& in = binding.input;
    if (binding.source == InputSource::Keyboard) {
        if (in.rfind("Key", 0) == 0 && in.size() == 4) {
            // "KeyW" -> "W".
            return in.substr(3);
        }
        return in;  // "Space", "Enter", "Ctrl"...
    }
    if (binding.source == InputSource::Mouse) {
        if (in == "MouseLeft") return "Left Mouse";
        if (in == "MouseRight") return "Right Mouse";
        if (in == "MouseMiddle") return "Middle Mouse";
        return in;
    }
    if (binding.source == InputSource::Gamepad) {
        if (in.rfind("Pad", 0) == 0 && in.size() == 4) {
            return "Gamepad " + in.substr(3);
        }
        if (in == "AxisLX") return "Left Stick X";
        if (in == "AxisLY") return "Left Stick Y";
        if (in == "AxisRX") return "Right Stick X";
        if (in == "AxisRY") return "Right Stick Y";
        return in;
    }
    return in;
}

class ShortcutDocRuntime final : public IShortcutDoc {
public:
    explicit ShortcutDocRuntime(const ShortcutDocSpec& spec) : spec_(spec) {
        for (const ShortcutEntry& entry : spec_.entries) {
            labels_[entry.action] = entry;
        }
    }

    std::string humanize(const engine::input::InputBinding& binding)
        const override {
        using engine::input::InputSource;
        std::ostringstream out;
        if (binding.source == InputSource::Keyboard ||
            binding.source == InputSource::Mouse) {
            out << readable_input(binding);
        } else if (binding.source == InputSource::Gamepad) {
            out << readable_input(binding);
            if (binding.axis != 0) out << " (" << binding.axis << ")";
        } else {
            out << source_name(binding.source) << ": " << readable_input(binding);
        }
        if (binding.scale < 0.0) out << " (inverted)";
        else if (binding.scale > 1.0) out << " (x" << binding.scale << ")";
        return out.str();
    }

    std::string document(const engine::input::ActionMapSpec& map,
                         std::string& errorOut) const override {
        errorOut.clear();
        if (!map.validate(errorOut)) return "";

        std::ostringstream out;
        if (!spec_.title.empty()) out << "# " << spec_.title << "\n\n";

        for (const engine::input::ActionBinding& action : map.actions) {
            const auto labelIt = labels_.find(action.action);
            const std::string label =
                labelIt == labels_.end() || labelIt->second.label.empty()
                    ? action.action
                    : labelIt->second.label;
            out << "## " << label << "\n";
            if (labelIt != labels_.end() &&
                !labelIt->second.description.empty()) {
                out << labelIt->second.description << "\n";
            }
            if (action.bindings.empty()) {
                out << "- (no bindings)\n";
            } else {
                for (const engine::input::InputBinding& binding :
                     action.bindings) {
                    out << "- " << humanize(binding) << "\n";
                }
            }
            out << "\n";
        }

        // Entries whose action is not in the map: visible, never dropped.
        std::vector<std::string> unbound;
        for (const ShortcutEntry& entry : spec_.entries) {
            bool found = false;
            for (const engine::input::ActionBinding& action : map.actions) {
                if (action.action == entry.action) { found = true; break; }
            }
            if (!found) unbound.push_back(entry.action);
        }
        if (!unbound.empty()) {
            out << "## UNBOUND\n";
            for (const std::string& action : unbound) {
                out << "- " << action << " (not in the action map)\n";
            }
        }
        return out.str();
    }

    const ShortcutDocSpec& spec() const override { return spec_; }

private:
    ShortcutDocSpec spec_;
    std::map<std::string, ShortcutEntry> labels_;
};

}  // namespace

std::unique_ptr<IShortcutDoc> create_shortcut_doc(const ShortcutDocSpec& spec,
                                                  std::string& errorOut) {
    errorOut.clear();
    if (!spec.validate(errorOut)) return nullptr;
    return std::make_unique<ShortcutDocRuntime>(spec);
}

}  // namespace editor
}  // namespace engine
