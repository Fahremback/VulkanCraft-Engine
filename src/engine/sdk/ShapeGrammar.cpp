// ShapeGrammar.cpp
//
// SDK adapter for engine/procgen/IShapeGrammar.hpp (META section 18 /
// FALTANTES item 14: permissive shape grammar for voxel mass models). The
// language is an original design inspired by the public concepts of shape
// grammars / CGA (Stiny 1975, Muller 2006); the catalog clone shape-ml is a
// GPL reference only and is NEVER compiled or included — this TU is fully
// self-contained (no backend).
//
// Semantics: rules are pure functions scope -> emitted boxes. Execution
// starts at "Axiom" with scope (0,0,0) size (1,1,1); `size`/`extrude`
// replace the scope size, `mass` emits a box over the current scope,
// `split` lays parts along an axis (clipped to the scope extent) and runs a
// named rule per part, `call` runs a named rule on the current scope. A
// recursion depth limit stops runaway grammars. No randomness: two runs
// (or two runner instances) with the same grammar produce bit-identical
// boxes.

#include "engine/procgen/IShapeGrammar.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {
namespace procgen {

namespace {

constexpr int kMaxDepth = 64;

struct Scope {
    int x{ 0 };
    int y{ 0 };
    int z{ 0 };
    int w{ 1 };
    int h{ 1 };
    int d{ 1 };
};

// Rule lookup index (deterministic: lookup only, order never iterated).
using RuleIndex = std::unordered_map<std::string, const GrammarRule*>;

bool index_rules(const ShapeGrammar& grammar, RuleIndex& index,
                 std::string& errorOut) {
    index.clear();
    for (const GrammarRule& rule : grammar.rules) {
        if (rule.name.empty()) {
            errorOut = "shape grammar: rule with empty name";
            return false;
        }
        if (index.count(rule.name) != 0) {
            errorOut = "shape grammar: duplicate rule name '" + rule.name + "'";
            return false;
        }
        index.emplace(rule.name, &rule);
    }
    return true;
}

bool is_valid_axis(char axis) {
    return axis == 'x' || axis == 'y' || axis == 'z';
}

bool validate_rule(const GrammarRule& rule, const RuleIndex& index,
                   std::string& errorOut) {
    for (const GrammarOp& op : rule.ops) {
        if (const auto* s = std::get_if<GrammarOpSize>(&op)) {
            if (s->width <= 0 || s->height <= 0 || s->depth <= 0) {
                errorOut = "shape grammar: rule '" + rule.name +
                           "' has a size op with non-positive dimensions";
                return false;
            }
        } else if (const auto* e = std::get_if<GrammarOpExtrude>(&op)) {
            if (e->height <= 0) {
                errorOut = "shape grammar: rule '" + rule.name +
                           "' has an extrude op with non-positive height";
                return false;
            }
        } else if (const auto* sp = std::get_if<GrammarOpSplit>(&op)) {
            if (!is_valid_axis(sp->axis)) {
                errorOut = "shape grammar: rule '" + rule.name +
                           "' has a split op with invalid axis '" +
                           std::string(1, sp->axis) + "'";
                return false;
            }
            if (sp->sizes.size() != sp->rules.size()) {
                errorOut = "shape grammar: rule '" + rule.name +
                           "' split op sizes/rules count mismatch";
                return false;
            }
            for (const int size : sp->sizes) {
                if (size <= 0) {
                    errorOut = "shape grammar: rule '" + rule.name +
                               "' split op has a non-positive size";
                    return false;
                }
            }
            for (const std::string& ref : sp->rules) {
                if (index.count(ref) == 0) {
                    errorOut = "shape grammar: rule '" + rule.name +
                               "' references unknown rule '" + ref + "'";
                    return false;
                }
            }
        } else if (const auto* c = std::get_if<GrammarOpCall>(&op)) {
            if (index.count(c->rule) == 0) {
                errorOut = "shape grammar: rule '" + rule.name +
                           "' references unknown rule '" + c->rule + "'";
                return false;
            }
        }
    }
    return true;
}

bool validate_grammar(const ShapeGrammar& grammar, RuleIndex& index,
                      std::string& errorOut) {
    if (!index_rules(grammar, index, errorOut)) {
        return false;
    }
    if (index.count("Axiom") == 0) {
        errorOut = "shape grammar: no 'Axiom' rule";
        return false;
    }
    for (const GrammarRule& rule : grammar.rules) {
        if (!validate_rule(rule, index, errorOut)) {
            return false;
        }
    }
    return true;
}

}  // namespace

class ShapeGrammarRunner final : public IShapeGrammarRunner {
public:
    bool run(const ShapeGrammar& grammar, GrammarResult& out,
             std::string& errorOut) override {
        out.boxes.clear();
        RuleIndex index;
        if (!validate_grammar(grammar, index, errorOut)) {
            return false;
        }
        const GrammarRule& axiom = *index.at("Axiom");
        Scope root;
        if (!execute(axiom, root, 0, index, out, errorOut)) {
            return false;
        }
        return true;
    }

    bool validate(const ShapeGrammar& grammar,
                  std::string& errorOut) const override {
        RuleIndex index;
        return validate_grammar(grammar, index, errorOut);
    }

    bool serialize(const ShapeGrammar& grammar, std::string& out) const override {
        std::ostringstream ss;
        ss << "{\"version\":1,\"rules\":[";
        for (std::size_t r = 0; r < grammar.rules.size(); ++r) {
            if (r > 0) {
                ss << ',';
            }
            const GrammarRule& rule = grammar.rules[r];
            ss << "{\"name\":\"" << rule.name << "\",\"ops\":[";
            for (std::size_t o = 0; o < rule.ops.size(); ++o) {
                if (o > 0) {
                    ss << ',';
                }
                write_op(rule.ops[o], ss);
            }
            ss << "]}";
        }
        ss << "]}";
        out = ss.str();
        return true;
    }

    bool deserialize(const std::string& json, ShapeGrammar& out,
                     std::string& errorOut) const override {
        sdk::JsonValue document;
        if (!sdk::json_parse(json, document, errorOut)) {
            errorOut = "shape grammar: malformed asset - " + errorOut;
            return false;
        }
        if (!document.is_object()) {
            errorOut = "shape grammar: asset must be a JSON object";
            return false;
        }
        const sdk::JsonValue* version = document.field("version");
        if (version == nullptr || version->kind != sdk::JsonValue::Kind::Number ||
            static_cast<int>(version->number) != 1) {
            errorOut = "shape grammar: unsupported asset version";
            return false;
        }
        const sdk::JsonValue* rules = document.field("rules");
        if (rules == nullptr || !rules->is_array()) {
            errorOut = "shape grammar: asset has no \"rules\" array";
            return false;
        }
        ShapeGrammar parsed;
        for (std::size_t i = 0; i < rules->array.size(); ++i) {
            const sdk::JsonValue& entry = rules->array[i];
            if (!entry.is_object()) {
                errorOut = "shape grammar: rule " + std::to_string(i) +
                           " must be an object";
                return false;
            }
            GrammarRule rule;
            rule.name = sdk::json_string(entry, "name", "");
            if (rule.name.empty()) {
                errorOut = "shape grammar: rule " + std::to_string(i) +
                           " has no name";
                return false;
            }
            const sdk::JsonValue* ops = entry.field("ops");
            if (ops == nullptr || !ops->is_array()) {
                errorOut = "shape grammar: rule '" + rule.name +
                           "' has no \"ops\" array";
                return false;
            }
            for (std::size_t k = 0; k < ops->array.size(); ++k) {
                const sdk::JsonValue& opEntry = ops->array[k];
                GrammarOp op;
                if (!parse_op(opEntry, rule.name, op, errorOut)) {
                    return false;
                }
                rule.ops.push_back(std::move(op));
            }
            parsed.rules.push_back(std::move(rule));
        }
        // All-or-nothing: only commit a valid grammar.
        RuleIndex index;
        if (!validate_grammar(parsed, index, errorOut)) {
            return false;
        }
        out = std::move(parsed);
        return true;
    }

private:
    bool execute(const GrammarRule& rule, const Scope& scope, int depth,
                 const RuleIndex& index, GrammarResult& out,
                 std::string& errorOut) {
        if (depth > kMaxDepth) {
            errorOut = "shape grammar: recursion too deep (rule '" + rule.name +
                       "'; grammar is not terminating)";
            return false;
        }
        Scope current = scope;
        for (const GrammarOp& op : rule.ops) {
            if (const auto* s = std::get_if<GrammarOpSize>(&op)) {
                current.w = s->width;
                current.h = s->height;
                current.d = s->depth;
            } else if (const auto* e = std::get_if<GrammarOpExtrude>(&op)) {
                current.h = e->height;
            } else if (const auto* m = std::get_if<GrammarOpMass>(&op)) {
                if (current.w > 0 && current.h > 0 && current.d > 0) {
                    out.boxes.push_back({ current.x, current.y, current.z,
                                          current.x + current.w,
                                          current.y + current.h,
                                          current.z + current.d, m->blockId });
                }
            } else if (const auto* sp = std::get_if<GrammarOpSplit>(&op)) {
                if (!split_into_parts(sp, current, index, depth, out,
                                      errorOut)) {
                    return false;
                }
            } else if (const auto* c = std::get_if<GrammarOpCall>(&op)) {
                const GrammarRule& target = *index.at(c->rule);
                if (!execute(target, current, depth + 1, index, out, errorOut)) {
                    return false;
                }
            }
        }
        return true;
    }

    bool split_into_parts(const GrammarOpSplit* split, const Scope& scope,
                          const RuleIndex& index, int depth,
                          GrammarResult& out, std::string& errorOut) {
        const int extent = (split->axis == 'x')   ? scope.w
                           : (split->axis == 'y') ? scope.h
                                                  : scope.d;
        int cursor = 0;
        for (std::size_t i = 0; i < split->sizes.size(); ++i) {
            const int partStart = cursor;
            cursor += split->sizes[i];  // layout always advances by the size
            const int partEnd = std::min(partStart + split->sizes[i], extent);
            if (partEnd <= partStart) {
                continue;  // fully beyond the scope extent: clipped
            }
            Scope child = scope;
            if (split->axis == 'x') {
                child.x += partStart;
                child.w = partEnd - partStart;
            } else if (split->axis == 'y') {
                child.y += partStart;
                child.h = partEnd - partStart;
            } else {
                child.z += partStart;
                child.d = partEnd - partStart;
            }
            const GrammarRule& target = *index.at(split->rules[i]);
            if (!execute(target, child, depth + 1, index, out, errorOut)) {
                return false;
            }
        }
        return true;
    }

    static void write_op(const GrammarOp& op, std::ostringstream& ss) {
        if (const auto* s = std::get_if<GrammarOpSize>(&op)) {
            ss << "{\"type\":\"size\",\"width\":" << s->width
               << ",\"height\":" << s->height << ",\"depth\":" << s->depth
               << '}';
        } else if (const auto* e = std::get_if<GrammarOpExtrude>(&op)) {
            ss << "{\"type\":\"extrude\",\"height\":" << e->height << '}';
        } else if (const auto* m = std::get_if<GrammarOpMass>(&op)) {
            ss << "{\"type\":\"mass\",\"blockId\":" << m->blockId << '}';
        } else if (const auto* sp = std::get_if<GrammarOpSplit>(&op)) {
            ss << "{\"type\":\"split\",\"axis\":\"" << sp->axis << "\",\"sizes\":[";
            for (std::size_t i = 0; i < sp->sizes.size(); ++i) {
                if (i > 0) {
                    ss << ',';
                }
                ss << sp->sizes[i];
            }
            ss << "],\"rules\":[";
            for (std::size_t i = 0; i < sp->rules.size(); ++i) {
                if (i > 0) {
                    ss << ',';
                }
                ss << '"' << sp->rules[i] << '"';
            }
            ss << "]}";
        } else if (const auto* c = std::get_if<GrammarOpCall>(&op)) {
            ss << "{\"type\":\"call\",\"rule\":\"" << c->rule << "\"}";
        }
    }

    static bool parse_op(const sdk::JsonValue& entry, const std::string& ruleName,
                         GrammarOp& out, std::string& errorOut) {
        if (!entry.is_object()) {
            errorOut = "shape grammar: rule '" + ruleName +
                       "' has a non-object op";
            return false;
        }
        const std::string type = sdk::json_string(entry, "type", "");
        if (type == "size") {
            GrammarOpSize s;
            s.width = static_cast<int>(sdk::json_number(entry, "width", 0.0));
            s.height = static_cast<int>(sdk::json_number(entry, "height", 0.0));
            s.depth = static_cast<int>(sdk::json_number(entry, "depth", 0.0));
            if (s.width <= 0 || s.height <= 0 || s.depth <= 0) {
                errorOut = "shape grammar: rule '" + ruleName +
                           "' size op needs positive width/height/depth";
                return false;
            }
            out = s;
        } else if (type == "extrude") {
            GrammarOpExtrude e;
            e.height = static_cast<int>(sdk::json_number(entry, "height", 0.0));
            if (e.height <= 0) {
                errorOut = "shape grammar: rule '" + ruleName +
                           "' extrude op needs positive height";
                return false;
            }
            out = e;
        } else if (type == "mass") {
            GrammarOpMass m;
            m.blockId = static_cast<std::uint32_t>(
                sdk::json_number(entry, "blockId", 0.0));
            out = m;
        } else if (type == "split") {
            GrammarOpSplit sp;
            const std::string axis = sdk::json_string(entry, "axis", "");
            if (axis.size() != 1 || !is_valid_axis(axis[0])) {
                errorOut = "shape grammar: rule '" + ruleName +
                           "' split op needs axis x/y/z";
                return false;
            }
            sp.axis = axis[0];
            const std::vector<double> sizes =
                sdk::json_number_array(entry, "sizes");
            const std::vector<std::string> rules =
                sdk::json_string_array(entry, "rules");
            if (sizes.empty() || sizes.size() != rules.size()) {
                errorOut = "shape grammar: rule '" + ruleName +
                           "' split op needs matching non-empty sizes/rules";
                return false;
            }
            for (const double size : sizes) {
                if (size <= 0.0 ||
                    size != static_cast<double>(static_cast<int>(size))) {
                    errorOut = "shape grammar: rule '" + ruleName +
                               "' split op sizes must be positive integers";
                    return false;
                }
                sp.sizes.push_back(static_cast<int>(size));
            }
            sp.rules = rules;
            out = sp;
        } else if (type == "call") {
            GrammarOpCall c;
            c.rule = sdk::json_string(entry, "rule", "");
            if (c.rule.empty()) {
                errorOut = "shape grammar: rule '" + ruleName +
                           "' call op needs a rule name";
                return false;
            }
            out = c;
        } else {
            errorOut = "shape grammar: rule '" + ruleName +
                       "' has unknown op type '" + type + "'";
            return false;
        }
        return true;
    }
};

}  // namespace procgen
}  // namespace engine

namespace engine {
namespace procgen {

std::shared_ptr<IShapeGrammarRunner> create_shape_grammar_runner() {
    return std::make_shared<ShapeGrammarRunner>();
}

}  // namespace procgen
}  // namespace engine
