#include "engine/gameplay/IFaction.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace engine {
namespace gameplay {
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

bool is_uint64(const sdk::JsonValue& v) {
    return v.kind == sdk::JsonValue::Kind::Number && v.number >= 0.0 &&
           v.number == std::floor(v.number);
}

const char* relation_name(FactionRelation r) {
    switch (r) {
        case FactionRelation::Friendly: return "friendly";
        case FactionRelation::Hostile: return "hostile";
        case FactionRelation::Neutral: return "neutral";
    }
    return "neutral";
}

bool relation_from_name(const std::string& name, FactionRelation& out) {
    if (name == "friendly") { out = FactionRelation::Friendly; return true; }
    if (name == "hostile") { out = FactionRelation::Hostile; return true; }
    if (name == "neutral") { out = FactionRelation::Neutral; return true; }
    return false;
}

// Chave canônica simétrica (ordem lexicográfica) para o mapa de relações.
std::pair<std::string, std::string> relation_key(const std::string& a,
                                                 const std::string& b) {
    return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
}

}  // namespace

bool FactionSpec::validate(std::string& errorOut) const {
    std::map<std::string, bool> ids;
    for (const auto& t : teams) {
        if (t.empty()) {
            errorOut = "team id must be non-empty";
            return false;
        }
        if (ids.count(t)) {
            errorOut = "duplicate team id \"" + t + "\"";
            return false;
        }
        ids[t] = true;
    }
    for (const auto& r : relations) {
        if (!ids.count(r.a)) {
            errorOut = "relation references unknown team \"" + r.a + "\"";
            return false;
        }
        if (!ids.count(r.b)) {
            errorOut = "relation references unknown team \"" + r.b + "\"";
            return false;
        }
    }
    errorOut.clear();
    return true;
}

bool FactionSpec::load_from_json(const std::string& json, std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(json, doc, errorOut)) {
        return false;
    }
    if (!doc.is_object()) {
        errorOut = "faction spec must be an object";
        return false;
    }
    const sdk::JsonValue* version = doc.field("version");
    if (version != nullptr &&
        (!is_uint64(*version) || static_cast<int>(version->number) != 1)) {
        errorOut = "unsupported faction spec version";
        return false;
    }
    FactionSpec candidate;
    const sdk::JsonValue* teamsField = doc.field("teams");
    if (teamsField != nullptr) {
        if (!teamsField->is_array()) {
            errorOut = "teams must be an array";
            return false;
        }
        for (const auto& item : teamsField->array) {
            if (item.kind != sdk::JsonValue::Kind::String) {
                errorOut = "team ids must be strings";
                return false;
            }
            candidate.teams.push_back(item.string);
        }
    }
    const sdk::JsonValue* relField = doc.field("relations");
    if (relField != nullptr) {
        if (!relField->is_array()) {
            errorOut = "relations must be an array";
            return false;
        }
        for (const auto& item : relField->array) {
            if (!item.is_object()) {
                errorOut = "relation entries must be objects";
                return false;
            }
            FactionSpec::Relation r;
            const sdk::JsonValue* a = item.field("a");
            const sdk::JsonValue* b = item.field("b");
            const sdk::JsonValue* rel = item.field("relation");
            if (a == nullptr || a->kind != sdk::JsonValue::Kind::String ||
                b == nullptr || b->kind != sdk::JsonValue::Kind::String ||
                rel == nullptr || rel->kind != sdk::JsonValue::Kind::String) {
                errorOut = "relation needs string a/b/relation";
                return false;
            }
            r.a = a->string;
            r.b = b->string;
            if (!relation_from_name(rel->string, r.relation)) {
                errorOut = "unknown relation \"" + rel->string + "\"";
                return false;
            }
            candidate.relations.push_back(r);
        }
    }
    if (!candidate.validate(errorOut)) {
        return false;
    }
    *this = std::move(candidate);
    return true;
}

std::string FactionSpec::to_json() const {
    std::ostringstream out;
    out << "{\"version\":1,\"teams\":[";
    for (std::size_t i = 0; i < teams.size(); ++i) {
        if (i) out << ",";
        out << "\"" << json_escape(teams[i]) << "\"";
    }
    out << "],\"relations\":[";
    for (std::size_t i = 0; i < relations.size(); ++i) {
        if (i) out << ",";
        out << "{\"a\":\"" << json_escape(relations[i].a) << "\",\"b\":\""
            << json_escape(relations[i].b) << "\",\"relation\":\""
            << relation_name(relations[i].relation) << "\"}";
    }
    out << "]}";
    return out.str();
}

namespace {

class Faction final : public IFaction {
public:
    bool configure(const FactionSpec& spec, std::string& errorOut) override {
        if (!spec.validate(errorOut)) {
            return false;
        }
        teams_.clear();
        relations_.clear();
        for (const auto& t : spec.teams) {
            teams_.insert(t);
        }
        for (const auto& r : spec.relations) {
            relations_[relation_key(r.a, r.b)] = r.relation;
        }
        return true;
    }

    bool register_team(const std::string& team, std::string& errorOut) override {
        if (team.empty()) {
            errorOut = "team id must be non-empty";
            return false;
        }
        if (teams_.count(team)) {
            errorOut = "duplicate team id \"" + team + "\"";
            return false;
        }
        teams_.insert(team);
        errorOut.clear();
        return true;
    }

    bool set_relation(const std::string& a, const std::string& b,
                      FactionRelation relation, std::string& errorOut) override {
        if (!teams_.count(a) || !teams_.count(b)) {
            errorOut = "set_relation references unknown team";
            return false;
        }
        relations_[relation_key(a, b)] = relation;
        errorOut.clear();
        return true;
    }

    FactionRelation relation(const std::string& a, const std::string& b) const override {
        if (a == b) {
            return FactionRelation::Friendly;  // uma equipe é amigável consigo
        }
        const auto it = relations_.find(relation_key(a, b));
        if (it == relations_.end()) {
            return FactionRelation::Neutral;
        }
        return it->second;
    }

    bool is_hostile(const std::string& a, const std::string& b) const override {
        return relation(a, b) == FactionRelation::Hostile;
    }

    bool is_friendly(const std::string& a, const std::string& b) const override {
        return relation(a, b) == FactionRelation::Friendly;
    }

    std::vector<std::string> teams() const override {
        return std::vector<std::string>(teams_.begin(), teams_.end());  // set ordena
    }

    std::string serialize_state() const override {
        std::ostringstream out;
        out << "{\"teams\":[";
        bool first = true;
        for (const auto& t : teams_) {
            if (!first) out << ",";
            first = false;
            out << "\"" << json_escape(t) << "\"";
        }
        out << "],\"relations\":[";
        // ordena o mapa (key é um pair ordenado → ordem determinística)
        first = true;
        for (const auto& kv : relations_) {
            if (!first) out << ",";
            first = false;
            out << "{\"a\":\"" << json_escape(kv.first.first) << "\",\"b\":\""
                << json_escape(kv.first.second) << "\",\"relation\":\""
                << relation_name(kv.second) << "\"}";
        }
        out << "]}";
        return out.str();
    }

    bool deserialize_state(const std::string& json, std::string& errorOut) override {
        FactionSpec spec;
        if (!spec.load_from_json(json, errorOut)) {
            return false;
        }
        return configure(spec, errorOut);
    }

private:
    std::set<std::string> teams_;
    std::map<std::pair<std::string, std::string>, FactionRelation> relations_;
};

}  // namespace

std::unique_ptr<IFaction> create_faction() {
    return std::make_unique<Faction>();
}

}  // namespace gameplay
}  // namespace engine
