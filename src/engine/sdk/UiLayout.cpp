// UiLayout.cpp — the ONLY TU with the data-driven UI layout runtime
// (agente 2 §A item 1). Pure and deterministic: layout() resolves a widget
// tree into absolute rects and eval_binding() evaluates data-binding
// expressions; no window, no GPU, no wall clock, no RNG. JSON parse/emit uses
// the shared RegistryJson helpers.

#include "engine/ui/ILayout.hpp"

#include "RegistryJson.hpp"

#include <cmath>
#include <cctype>
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

const char* direction_name(LayoutDirection d) {
    return d == LayoutDirection::Row ? "row" : "column";
}

bool direction_from_name(const std::string& name, LayoutDirection& out) {
    if (name == "row") { out = LayoutDirection::Row; return true; }
    if (name == "column") { out = LayoutDirection::Column; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Data-binding expression evaluator
// ---------------------------------------------------------------------------

class ExprParser {
public:
    explicit ExprParser(const std::string& text, const UiStore& store,
                        std::string& errorOut)
        : text_(text), store_(store), errorOut_(errorOut) {}

    bool parse(UiValue& out) {
        pos_ = 0;
        skip_ws();
        if (!add(out)) return false;
        skip_ws();
        if (pos_ != text_.size()) {
            errorOut_ = "unexpected trailing input in binding expression";
            return false;
        }
        return true;
    }

private:
    const std::string& text_;
    const UiStore& store_;
    std::string& errorOut_;
    std::size_t pos_{ 0 };

    void skip_ws() {
        while (pos_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    bool add(UiValue& out) {
        if (!mul(out)) return false;
        while (true) {
            skip_ws();
            if (pos_ >= text_.size()) return true;
            const char op = text_[pos_];
            if (op != '+' && op != '-') return true;
            ++pos_;
            UiValue rhs;
            if (!mul(rhs)) return false;
            if (out.kind != UiValue::Kind::Number ||
                rhs.kind != UiValue::Kind::Number) {
                errorOut_ = "arithmetic requires number operands";
                return false;
            }
            out.number = (op == '+') ? out.number + rhs.number
                                     : out.number - rhs.number;
        }
    }

    bool mul(UiValue& out) {
        if (!unary(out)) return false;
        while (true) {
            skip_ws();
            if (pos_ >= text_.size()) return true;
            const char op = text_[pos_];
            if (op != '*' && op != '/') return true;
            ++pos_;
            UiValue rhs;
            if (!unary(rhs)) return false;
            if (out.kind != UiValue::Kind::Number ||
                rhs.kind != UiValue::Kind::Number) {
                errorOut_ = "arithmetic requires number operands";
                return false;
            }
            if (op == '/') {
                if (rhs.number == 0.0) {
                    errorOut_ = "division by zero in binding expression";
                    return false;
                }
                out.number /= rhs.number;
            } else {
                out.number *= rhs.number;
            }
        }
    }

    bool unary(UiValue& out) {
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '-') {
            ++pos_;
            if (!unary(out)) return false;
            if (out.kind != UiValue::Kind::Number) {
                errorOut_ = "unary minus requires a number operand";
                return false;
            }
            out.number = -out.number;
            return true;
        }
        return primary(out);
    }

    bool primary(UiValue& out) {
        skip_ws();
        if (pos_ >= text_.size()) {
            errorOut_ = "unexpected end of binding expression";
            return false;
        }
        const char c = text_[pos_];

        // Parenthesized expression.
        if (c == '(') {
            ++pos_;
            if (!add(out)) return false;
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != ')') {
                errorOut_ = "missing ')' in binding expression";
                return false;
            }
            ++pos_;
            return true;
        }

        // Store reference.
        if (c == '$') {
            ++pos_;
            const std::size_t start = pos_;
            while (pos_ < text_.size() &&
                   (std::isalnum(static_cast<unsigned char>(text_[pos_])) ||
                    text_[pos_] == '_' || text_[pos_] == '.')) {
                ++pos_;
            }
            if (pos_ == start) {
                errorOut_ = "empty store reference in binding expression";
                return false;
            }
            const std::string key = text_.substr(start, pos_ - start);
            const auto found = store_.find(key);
            if (found == store_.end()) {
                errorOut_ = "unknown store key: " + key;
                return false;
            }
            out = found->second;
            return true;
        }

        // String literal.
        if (c == '"') {
            ++pos_;
            std::string value;
            while (pos_ < text_.size() && text_[pos_] != '"') {
                if (text_[pos_] == '\\' && pos_ + 1 < text_.size()) {
                    const char esc = text_[pos_ + 1];
                    if (esc == 'n') value += '\n';
                    else if (esc == 't') value += '\t';
                    else if (esc == 'r') value += '\r';
                    else value += esc;
                    pos_ += 2;
                } else {
                    value += text_[pos_];
                    ++pos_;
                }
            }
            if (pos_ >= text_.size()) {
                errorOut_ = "unterminated string in binding expression";
                return false;
            }
            ++pos_; // closing quote
            out = UiValue{ UiValue::Kind::String, 0.0, false, value };
            return true;
        }

        // Boolean literal.
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            out = UiValue{ UiValue::Kind::Bool, 0.0, true, {} };
            return true;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            out = UiValue{ UiValue::Kind::Bool, 0.0, false, {} };
            return true;
        }

        // Number literal.
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            const std::size_t start = pos_;
            while (pos_ < text_.size() &&
                   (std::isdigit(static_cast<unsigned char>(text_[pos_])) ||
                    text_[pos_] == '.' || text_[pos_] == 'e' ||
                    text_[pos_] == 'E' || text_[pos_] == '+' ||
                    text_[pos_] == '-')) {
                ++pos_;
            }
            const std::string token = text_.substr(start, pos_ - start);
            char* end = nullptr;
            const double value = std::strtod(token.c_str(), &end);
            if (end == token.c_str() || !is_finite(value)) {
                errorOut_ = "invalid number in binding expression: " + token;
                return false;
            }
            out = UiValue{ UiValue::Kind::Number, value, false, {} };
            return true;
        }

        errorOut_ = std::string("unexpected character in binding expression: ") + c;
        return false;
    }
};

// ---------------------------------------------------------------------------
// Spec validation + JSON
// ---------------------------------------------------------------------------

bool validate_node(const LayoutNode& node, std::string& errorOut) {
    if (node.id.empty()) {
        errorOut = "layout node has an empty id";
        return false;
    }
    if (!is_finite(node.weight) || node.weight <= 0.0) {
        errorOut = "node \"" + node.id + "\" weight must be > 0";
        return false;
    }
    if (!is_finite(node.min_w) || node.min_w < 0.0) {
        errorOut = "node \"" + node.id + "\" min_w must be finite and >= 0";
        return false;
    }
    if (!is_finite(node.min_h) || node.min_h < 0.0) {
        errorOut = "node \"" + node.id + "\" min_h must be finite and >= 0";
        return false;
    }
    if (node.max_w >= 0.0 && node.max_w < node.min_w) {
        errorOut = "node \"" + node.id + "\" max_w < min_w";
        return false;
    }
    if (node.max_h >= 0.0 && node.max_h < node.min_h) {
        errorOut = "node \"" + node.id + "\" max_h < min_h";
        return false;
    }
    if (!is_finite(node.margin) || node.margin < 0.0) {
        errorOut = "node \"" + node.id + "\" margin must be finite and >= 0";
        return false;
    }
    if (!is_finite(node.padding) || node.padding < 0.0) {
        errorOut = "node \"" + node.id + "\" padding must be finite and >= 0";
        return false;
    }
    for (const LayoutNode& child : node.children) {
        if (!validate_node(child, errorOut)) return false;
    }
    return true;
}

bool collect_ids(const LayoutNode& node, std::map<std::string, bool>& seen,
                 std::string& errorOut) {
    if (seen.count(node.id)) {
        errorOut = "duplicate node id: " + node.id;
        return false;
    }
    seen[node.id] = true;
    for (const LayoutNode& child : node.children) {
        if (!collect_ids(child, seen, errorOut)) return false;
    }
    return true;
}

}  // namespace

bool UiLayoutSpec::validate(std::string& errorOut) const {
    errorOut.clear();
    if (version != 1) {
        errorOut = "unsupported ui layout version";
        return false;
    }
    if (root.empty()) {
        errorOut = "layout spec has an empty root id";
        return false;
    }
    if (!validate_node(tree, errorOut)) return false;
    std::map<std::string, bool> seen;
    if (!collect_ids(tree, seen, errorOut)) return false;
    if (!seen.count(root)) {
        errorOut = "root node \"" + root + "\" not found in the tree";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// JSON round-trip (recursive)
// ---------------------------------------------------------------------------

namespace {

std::string node_to_json(const LayoutNode& node);

std::string num(double v) {
    std::ostringstream out;
    out.precision(9);
    out << v;
    return out.str();
}

std::string node_to_json(const LayoutNode& node) {
    std::ostringstream out;
    out << "{\"id\":\"" << json_escape(node.id) << "\",\"direction\":\""
        << direction_name(node.direction) << "\",\"weight\":" << num(node.weight)
        << ",\"min_w\":" << num(node.min_w) << ",\"max_w\":" << num(node.max_w)
        << ",\"min_h\":" << num(node.min_h) << ",\"max_h\":" << num(node.max_h)
        << ",\"margin\":" << num(node.margin) << ",\"padding\":" << num(node.padding);
    if (!node.text_binding.empty()) {
        out << ",\"text_binding\":\"" << json_escape(node.text_binding) << "\"";
    }
    if (!node.visible_binding.empty()) {
        out << ",\"visible_binding\":\"" << json_escape(node.visible_binding) << "\"";
    }
    out << ",\"children\":[";
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        if (i) out << ',';
        out << node_to_json(node.children[i]);
    }
    out << "]}";
    return out.str();
}

bool node_from_json(const sdk::JsonValue& obj, LayoutNode& out,
                    std::string& errorOut) {
    if (!obj.is_object()) {
        errorOut = "layout node must be an object";
        return false;
    }
    out.id = sdk::json_string(obj, "id", "");
    const std::string dir = sdk::json_string(obj, "direction", "column");
    if (!direction_from_name(dir, out.direction)) {
        errorOut = "unknown layout direction: " + dir;
        return false;
    }
    out.weight = sdk::json_number(obj, "weight", 1.0);
    out.min_w = sdk::json_number(obj, "min_w", 0.0);
    out.max_w = sdk::json_number(obj, "max_w", -1.0);
    out.min_h = sdk::json_number(obj, "min_h", 0.0);
    out.max_h = sdk::json_number(obj, "max_h", -1.0);
    out.margin = sdk::json_number(obj, "margin", 0.0);
    out.padding = sdk::json_number(obj, "padding", 0.0);
    out.text_binding = sdk::json_string(obj, "text_binding", "");
    out.visible_binding = sdk::json_string(obj, "visible_binding", "");
    const sdk::JsonValue* childrenField = obj.field("children");
    if (childrenField != nullptr) {
        if (!childrenField->is_array()) {
            errorOut = "node \"" + out.id + "\" children must be an array";
            return false;
        }
        out.children.reserve(childrenField->array.size());
        for (const sdk::JsonValue& childObj : childrenField->array) {
            LayoutNode child;
            if (!node_from_json(childObj, child, errorOut)) return false;
            out.children.push_back(std::move(child));
        }
    }
    return true;
}

}  // namespace

std::string UiLayoutSpec::to_json() const {
    std::ostringstream out;
    out << "{\"version\":1,\"root\":\"" << json_escape(root)
        << "\",\"tree\":" << node_to_json(tree) << "}";
    return out.str();
}

bool UiLayoutSpec::load_from_json(const std::string& jsonText,
                                  std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(jsonText, doc, errorOut)) return false;
    if (!doc.is_object()) {
        errorOut = "ui layout document must be an object";
        return false;
    }
    const int version = static_cast<int>(sdk::json_number(doc, "version", 1));
    if (version != 1) {
        errorOut = "unsupported ui layout version";
        return false;
    }
    UiLayoutSpec candidate;
    candidate.version = version;
    candidate.root = sdk::json_string(doc, "root", "");
    const sdk::JsonValue* treeField = doc.field("tree");
    if (treeField == nullptr) {
        errorOut = "ui layout missing tree";
        return false;
    }
    if (!node_from_json(*treeField, candidate.tree, errorOut)) return false;
    if (!candidate.validate(errorOut)) return false;
    *this = std::move(candidate);
    return true;
}

// ---------------------------------------------------------------------------
// eval_binding (free function)
// ---------------------------------------------------------------------------

bool eval_binding(const std::string& expr, const UiStore& store, UiValue& out,
                  std::string& errorOut) {
    errorOut.clear();
    ExprParser parser(expr, store, errorOut);
    return parser.parse(out);
}

// ---------------------------------------------------------------------------
// Layout runtime
// ---------------------------------------------------------------------------

namespace {

struct Resolver {
    std::string errorOut;

    bool run(const LayoutNode& root, double width, double height,
             const UiStore& store, std::vector<LayoutRect>& out) {
        if (!is_finite(width) || width < 0.0 || !is_finite(height) ||
            height < 0.0) {
            errorOut = "container size must be finite and >= 0";
            return false;
        }
        return place(root, width, height, store, out, /*root_rect*/ true);
    }

    // Places a node within (w, h) available space and recurses. Returns false
    // if a data binding fails to evaluate. Emits the node's rect first (pre-
    // order), then each visible child.
    bool place(const LayoutNode& node, double w, double h, const UiStore& store,
               std::vector<LayoutRect>& out, bool /*is_root*/) {
        // Visibility binding.
        bool visible = true;
        if (!node.visible_binding.empty()) {
            UiValue v;
            if (!eval_binding(node.visible_binding, store, v, errorOut)) {
                return false;
            }
            if (v.kind != UiValue::Kind::Bool) {
                errorOut = "visible binding must resolve to a bool for node \"" +
                           node.id + "\"";
                return false;
            }
            visible = v.boolean;
        }

        LayoutRect rect;
        rect.id = node.id;
        rect.visible = visible;
        if (!node.text_binding.empty()) {
            UiValue v;
            if (!eval_binding(node.text_binding, store, v, errorOut)) {
                return false;
            }
            switch (v.kind) {
                case UiValue::Kind::String: rect.text = v.string; break;
                case UiValue::Kind::Bool: rect.text = v.boolean ? "true" : "false"; break;
                case UiValue::Kind::Number: {
                    std::ostringstream s;
                    s.precision(9);
                    s << v.number;
                    rect.text = s.str();
                    break;
                }
            }
        }

        // Clamp the node's own box to min/max (responsive) before laying out
        // children.
        double box_w = w;
        double box_h = h;
        if (node.max_w >= 0.0 && box_w > node.max_w) box_w = node.max_w;
        if (node.max_h >= 0.0 && box_h > node.max_h) box_h = node.max_h;
        if (box_w < node.min_w) box_w = node.min_w;
        if (box_h < node.min_h) box_h = node.min_h;

        rect.w = box_w;
        rect.h = box_h;
        out.push_back(rect);

        // Hidden subtree: keep the rect (visible=false) but don't recurse.
        if (!visible) return true;

        // Content area (padding), then stack visible children.
        const double content_w = std::max(0.0, box_w - 2.0 * node.padding);
        const double content_h = std::max(0.0, box_h - 2.0 * node.padding);

        // Filter visible children and sum their weights.
        std::vector<const LayoutNode*> kids;
        double total_weight = 0.0;
        for (const LayoutNode& child : node.children) {
            bool child_visible = true;
            if (!child.visible_binding.empty()) {
                UiValue v;
                if (!eval_binding(child.visible_binding, store, v, errorOut)) {
                    return false;
                }
                if (v.kind != UiValue::Kind::Bool) {
                    errorOut = "visible binding must resolve to a bool for node \"" +
                               child.id + "\"";
                    return false;
                }
                child_visible = v.boolean;
            }
            if (!child_visible) {
                // Emit the hidden child's rect too (visible=false), pre-order.
                LayoutRect hidden;
                hidden.id = child.id;
                hidden.visible = false;
                out.push_back(hidden);
                continue;
            }
            kids.push_back(&child);
            total_weight += child.weight;
        }

        const bool row = (node.direction == LayoutDirection::Row);
        const double main_size = row ? content_w : content_h;
        const double cross_size = row ? content_h : content_w;

        double cursor = 0.0;
        for (const LayoutNode* child : kids) {
            const double share = (total_weight > 0.0)
                                     ? (child->weight / total_weight) * main_size
                                     : 0.0;
            double child_w = row ? share : cross_size;
            double child_h = row ? cross_size : share;

            // Clamp each child (responsive) and account for margins.
            const double m = 2.0 * child->margin;
            child_w = std::max(0.0, child_w - m);
            child_h = std::max(0.0, child_h - m);
            if (child->max_w >= 0.0 && child_w > child->max_w) child_w = child->max_w;
            if (child->max_h >= 0.0 && child_h > child->max_h) child_h = child->max_h;
            if (child_w < child->min_w) child_w = child->min_w;
            if (child_h < child->min_h) child_h = child->min_h;

            if (!place(*child, child_w, child_h, store, out, false)) {
                return false;
            }
            cursor += (row ? child_w : child_h) + m;
        }
        (void)cursor;
        return true;
    }
};

class UiLayoutRuntime final : public IUiLayout {
public:
    explicit UiLayoutRuntime(const UiLayoutSpec& spec) : spec_(spec) {}

    bool layout(double width, double height, const UiStore& store,
                std::vector<LayoutRect>& out, std::string& errorOut) override {
        out.clear();
        errorOut.clear();
        Resolver resolver;
        if (!resolver.run(spec_.tree, width, height, store, out)) {
            errorOut = resolver.errorOut;
            out.clear();
            return false;
        }
        return true;
    }

    const UiLayoutSpec& spec() const override { return spec_; }

private:
    UiLayoutSpec spec_;
};

}  // namespace

std::unique_ptr<IUiLayout> create_ui_layout(const UiLayoutSpec& spec,
                                            std::string& errorOut) {
    errorOut.clear();
    if (!spec.validate(errorOut)) return nullptr;
    return std::make_unique<UiLayoutRuntime>(spec);
}

}  // namespace ui
}  // namespace engine
