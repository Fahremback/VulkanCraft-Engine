// UiWidgets.cpp — the ONLY TU with the interactive-widget runtime (agente 2
// §A item 3). Bars/modals/focus-navigation are pure and deterministic: state
// comes from the UiStore via the shared eval_binding grammar; focus movement
// is a row-major grid walk. No window/GPU/input device. JSON parse/emit uses
// the shared RegistryJson helpers.

#include "engine/ui/IWidgets.hpp"

#include "engine/ui/ILayout.hpp" // eval_binding (shared grammar)

#include "RegistryJson.hpp"

#include <cmath>
#include <sstream>

namespace engine {
namespace ui {

namespace {

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
        }
    }
    return out;
}

bool is_finite(double v) { return std::isfinite(v); }

// Renders any UiValue as display text (matches ILayout's text rendering).
std::string render_value(const UiValue& v) {
    switch (v.kind) {
        case UiValue::Kind::String: return v.string;
        case UiValue::Kind::Bool: return v.boolean ? "true" : "false";
        case UiValue::Kind::Number: {
            std::ostringstream s;
            s.precision(9);
            s << v.number;
            return s.str();
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// WidgetsDoc validation + JSON
// ---------------------------------------------------------------------------

bool validate_bar(const UiBarSpec& bar, std::string& errorOut) {
    if (bar.id.empty()) {
        errorOut = "bar has an empty id";
        return false;
    }
    if (bar.value_binding.empty()) {
        errorOut = "bar \"" + bar.id + "\" has no value_binding";
        return false;
    }
    if (!is_finite(bar.min) || !is_finite(bar.max) || bar.max <= bar.min) {
        errorOut = "bar \"" + bar.id + "\" min/max must be finite with max > min";
        return false;
    }
    return true;
}

bool validate_modal(const UiModalSpec& modal, std::string& errorOut) {
    if (modal.id.empty()) {
        errorOut = "modal has an empty id";
        return false;
    }
    if (modal.title_binding.empty()) {
        errorOut = "modal \"" + modal.id + "\" has no title_binding";
        return false;
    }
    if (modal.visible_binding.empty()) {
        errorOut = "modal \"" + modal.id + "\" has no visible_binding";
        return false;
    }
    return true;
}

bool validate_focus(const UiFocusSpec& focus, std::string& errorOut) {
    if (focus.id.empty()) {
        errorOut = "focus grid has an empty id";
        return false;
    }
    if (focus.cols <= 0) {
        errorOut = "focus grid \"" + focus.id + "\" cols must be > 0";
        return false;
    }
    if (focus.ids.empty()) {
        errorOut = "focus grid \"" + focus.id + "\" has no navigable ids";
        return false;
    }
    for (std::size_t i = 0; i < focus.ids.size(); ++i) {
        if (focus.ids[i].empty()) {
            errorOut = "focus grid \"" + focus.id + "\" has an empty id";
            return false;
        }
        for (std::size_t j = i + 1; j < focus.ids.size(); ++j) {
            if (focus.ids[i] == focus.ids[j]) {
                errorOut = "focus grid \"" + focus.id + "\" has a duplicate id: " +
                           focus.ids[i];
                return false;
            }
        }
    }
    return true;
}

std::string bar_to_json(const UiBarSpec& bar) {
    std::ostringstream out;
    out.precision(9);
    out << "{\"id\":\"" << json_escape(bar.id) << "\",\"value_binding\":\""
        << json_escape(bar.value_binding) << "\"";
    if (!bar.label_binding.empty()) {
        out << ",\"label_binding\":\"" << json_escape(bar.label_binding) << "\"";
    }
    out << ",\"min\":" << bar.min << ",\"max\":" << bar.max
        << ",\"horizontal\":" << (bar.horizontal ? "true" : "false") << "}";
    return out.str();
}

std::string modal_to_json(const UiModalSpec& modal) {
    std::ostringstream out;
    out << "{\"id\":\"" << json_escape(modal.id) << "\",\"title_binding\":\""
        << json_escape(modal.title_binding) << "\",\"visible_binding\":\""
        << json_escape(modal.visible_binding) << "\",\"confirm_label\":\""
        << json_escape(modal.confirm_label) << "\",\"cancel_label\":\""
        << json_escape(modal.cancel_label) << "\"";
    if (!modal.on_confirm.empty()) {
        out << ",\"on_confirm\":\"" << json_escape(modal.on_confirm) << "\"";
    }
    if (!modal.on_cancel.empty()) {
        out << ",\"on_cancel\":\"" << json_escape(modal.on_cancel) << "\"";
    }
    out << "}";
    return out.str();
}

std::string focus_to_json(const UiFocusSpec& focus) {
    std::ostringstream out;
    out << "{\"id\":\"" << json_escape(focus.id) << "\",\"ids\":[";
    for (std::size_t i = 0; i < focus.ids.size(); ++i) {
        if (i) out << ',';
        out << "\"" << json_escape(focus.ids[i]) << "\"";
    }
    out << "],\"cols\":" << focus.cols
        << ",\"wrap\":" << (focus.wrap ? "true" : "false") << "}";
    return out.str();
}

bool bar_from_json(const sdk::JsonValue& obj, UiBarSpec& out,
                   std::string& errorOut) {
    if (!obj.is_object()) {
        errorOut = "bar must be an object";
        return false;
    }
    out.id = sdk::json_string(obj, "id", "");
    out.value_binding = sdk::json_string(obj, "value_binding", "");
    out.label_binding = sdk::json_string(obj, "label_binding", "");
    out.min = sdk::json_number(obj, "min", 0.0);
    out.max = sdk::json_number(obj, "max", 1.0);
    out.horizontal = sdk::json_bool(obj, "horizontal", true);
    return true;
}

bool modal_from_json(const sdk::JsonValue& obj, UiModalSpec& out,
                     std::string& errorOut) {
    if (!obj.is_object()) {
        errorOut = "modal must be an object";
        return false;
    }
    out.id = sdk::json_string(obj, "id", "");
    out.title_binding = sdk::json_string(obj, "title_binding", "");
    out.visible_binding = sdk::json_string(obj, "visible_binding", "");
    out.confirm_label = sdk::json_string(obj, "confirm_label", "Confirm");
    out.cancel_label = sdk::json_string(obj, "cancel_label", "Cancel");
    out.on_confirm = sdk::json_string(obj, "on_confirm", "");
    out.on_cancel = sdk::json_string(obj, "on_cancel", "");
    return true;
}

bool focus_from_json(const sdk::JsonValue& obj, UiFocusSpec& out,
                     std::string& errorOut) {
    if (!obj.is_object()) {
        errorOut = "focus grid must be an object";
        return false;
    }
    out.id = sdk::json_string(obj, "id", "");
    out.ids = sdk::json_string_array(obj, "ids");
    out.cols = static_cast<int>(sdk::json_number(obj, "cols", 1));
    out.wrap = sdk::json_bool(obj, "wrap", true);
    return true;
}

}  // namespace

bool WidgetsDoc::validate(std::string& errorOut) const {
    errorOut.clear();
    if (version != 1) {
        errorOut = "unsupported widgets document version";
        return false;
    }
    for (const UiBarSpec& bar : bars) {
        if (!validate_bar(bar, errorOut)) return false;
    }
    for (const UiModalSpec& modal : modals) {
        if (!validate_modal(modal, errorOut)) return false;
    }
    for (const UiFocusSpec& focus : focus) {
        if (!validate_focus(focus, errorOut)) return false;
    }
    return true;
}

std::string WidgetsDoc::to_json() const {
    std::ostringstream out;
    out << "{\"version\":1";
    out << ",\"bars\":[";
    for (std::size_t i = 0; i < bars.size(); ++i) {
        if (i) out << ',';
        out << bar_to_json(bars[i]);
    }
    out << "],\"modals\":[";
    for (std::size_t i = 0; i < modals.size(); ++i) {
        if (i) out << ',';
        out << modal_to_json(modals[i]);
    }
    out << "],\"focus\":[";
    for (std::size_t i = 0; i < focus.size(); ++i) {
        if (i) out << ',';
        out << focus_to_json(focus[i]);
    }
    out << "]}";
    return out.str();
}

bool WidgetsDoc::load_from_json(const std::string& jsonText,
                                std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(jsonText, doc, errorOut)) return false;
    if (!doc.is_object()) {
        errorOut = "widgets document must be an object";
        return false;
    }
    const int version = static_cast<int>(sdk::json_number(doc, "version", 1));
    if (version != 1) {
        errorOut = "unsupported widgets document version";
        return false;
    }
    WidgetsDoc candidate;
    candidate.version = version;

    const sdk::JsonValue* barsField = doc.field("bars");
    if (barsField != nullptr) {
        if (!barsField->is_array()) {
            errorOut = "bars must be an array";
            return false;
        }
        candidate.bars.reserve(barsField->array.size());
        for (const sdk::JsonValue& obj : barsField->array) {
            UiBarSpec bar;
            if (!bar_from_json(obj, bar, errorOut)) return false;
            candidate.bars.push_back(std::move(bar));
        }
    }

    const sdk::JsonValue* modalsField = doc.field("modals");
    if (modalsField != nullptr) {
        if (!modalsField->is_array()) {
            errorOut = "modals must be an array";
            return false;
        }
        candidate.modals.reserve(modalsField->array.size());
        for (const sdk::JsonValue& obj : modalsField->array) {
            UiModalSpec modal;
            if (!modal_from_json(obj, modal, errorOut)) return false;
            candidate.modals.push_back(std::move(modal));
        }
    }

    const sdk::JsonValue* focusField = doc.field("focus");
    if (focusField != nullptr) {
        if (!focusField->is_array()) {
            errorOut = "focus must be an array";
            return false;
        }
        candidate.focus.reserve(focusField->array.size());
        for (const sdk::JsonValue& obj : focusField->array) {
            UiFocusSpec grid;
            if (!focus_from_json(obj, grid, errorOut)) return false;
            candidate.focus.push_back(std::move(grid));
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

// Focus grid geometry helpers (row-major, `cols` columns).
// Returns the index of `from`, or n when unknown.
std::size_t focus_index(const UiFocusSpec& grid, const std::string& from) {
    for (std::size_t i = 0; i < grid.ids.size(); ++i) {
        if (grid.ids[i] == from) return i;
    }
    return grid.ids.size();
}

// One directional step in raw (row, col) coordinates. The grid has `rows`
// rows and `cols` cols (last row may be short).
struct Step {
    int dRow;
    int dCol;
};

Step step_for(FocusDirection dir) {
    switch (dir) {
        case FocusDirection::Up: return { -1, 0 };
        case FocusDirection::Down: return { 1, 0 };
        case FocusDirection::Left: return { 0, -1 };
        case FocusDirection::Right: return { 0, 1 };
    }
    return { 0, 0 };
}

class UiWidgetsRuntime final : public IUiWidgets {
public:
    explicit UiWidgetsRuntime(const WidgetsDoc& doc) : doc_(doc) {}

    std::vector<UiBarState> resolve_bars(const UiStore& store,
                                         std::string& errorOut) const override {
        errorOut.clear();
        std::vector<UiBarState> result;
        result.reserve(doc_.bars.size());
        for (const UiBarSpec& bar : doc_.bars) {
            UiBarState state;
            state.id = bar.id;
            UiValue v;
            if (!eval_binding(bar.value_binding, store, v, errorOut)) {
                return {};
            }
            if (v.kind != UiValue::Kind::Number) {
                errorOut = "bar \"" + bar.id + "\" value_binding must resolve to a number";
                return {};
            }
            state.value = v.number;
            const double range = bar.max - bar.min;
            const double raw = (state.value - bar.min) / range;
            state.fraction = raw < 0.0 ? 0.0 : (raw > 1.0 ? 1.0 : raw);
            if (!bar.label_binding.empty()) {
                UiValue label;
                if (!eval_binding(bar.label_binding, store, label, errorOut)) {
                    return {};
                }
                state.label = render_value(label);
            }
            result.push_back(std::move(state));
        }
        return result;
    }

    std::vector<UiModalState> resolve_modals(
        const UiStore& store, std::string& errorOut) const override {
        errorOut.clear();
        std::vector<UiModalState> result;
        result.reserve(doc_.modals.size());
        for (const UiModalSpec& modal : doc_.modals) {
            UiModalState state;
            state.id = modal.id;
            state.confirm_label = modal.confirm_label;
            state.cancel_label = modal.cancel_label;

            UiValue visible;
            if (!eval_binding(modal.visible_binding, store, visible, errorOut)) {
                return {};
            }
            if (visible.kind != UiValue::Kind::Bool) {
                errorOut = "modal \"" + modal.id +
                           "\" visible_binding must resolve to a bool";
                return {};
            }
            state.visible = visible.boolean;

            UiValue title;
            if (!eval_binding(modal.title_binding, store, title, errorOut)) {
                return {};
            }
            state.title = render_value(title);

            result.push_back(std::move(state));
        }
        return result;
    }

    UiFocusMove move_focus(const std::string& focusId, const std::string& from,
                           FocusDirection direction,
                           std::string& errorOut) const override {
        errorOut.clear();
        // Locate the grid.
        const UiFocusSpec* grid = nullptr;
        for (const UiFocusSpec& g : doc_.focus) {
            if (g.id == focusId) {
                grid = &g;
                break;
            }
        }
        if (grid == nullptr) {
            errorOut = "unknown focus grid: " + focusId;
            return {};
        }

        UiFocusMove result;
        result.from = from;

        // "" starts at the first id.
        if (from.empty()) {
            result.to = grid->ids.front();
            result.moved = true;
            return result;
        }

        const std::size_t n = grid->ids.size();
        const std::size_t index = focus_index(*grid, from);
        if (index == n) {
            errorOut = "unknown focused id: " + from;
            return {};
        }

        const int rows = (static_cast<int>(n) + grid->cols - 1) / grid->cols;
        const int row = static_cast<int>(index) / grid->cols;
        const int col = static_cast<int>(index) % grid->cols;
        const Step step = step_for(direction);

        int nrow = row + step.dRow;
        int ncol = col + step.dCol;

        const bool leftEdge = (ncol < 0);
        const bool rightEdge = (ncol >= grid->cols);
        const bool topEdge = (nrow < 0);
        const bool bottomEdge = (nrow >= rows);

        // Non-wrap: any edge crossing is refused.
        if (!grid->wrap) {
            if (leftEdge || rightEdge || topEdge || bottomEdge) {
                result.to = from; // stay
                result.moved = false;
                return result;
            }
        } else {
            // Wrap within the grid bounds.
            if (leftEdge) ncol = grid->cols - 1;
            if (rightEdge) ncol = 0;
            if (topEdge) nrow = rows - 1;
            if (bottomEdge) nrow = 0;
        }

        // The last row may be short: a wrap that lands past n (phantom cells)
        // is blocked deterministically — the focus stays where it was.
        const std::size_t target = static_cast<std::size_t>(nrow * grid->cols + ncol);
        if (target >= n) {
            result.to = from;
            result.moved = false;
            return result;
        }

        result.to = grid->ids[target];
        result.moved = true;
        return result;
    }

    const WidgetsDoc& spec() const override { return doc_; }

private:
    WidgetsDoc doc_;
};

}  // namespace

std::unique_ptr<IUiWidgets> create_ui_widgets(const WidgetsDoc& doc,
                                              std::string& errorOut) {
    errorOut.clear();
    if (!doc.validate(errorOut)) return nullptr;
    return std::make_unique<UiWidgetsRuntime>(doc);
}

}  // namespace ui
}  // namespace engine
