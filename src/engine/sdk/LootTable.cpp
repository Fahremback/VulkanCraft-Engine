#include "engine/registry/ILootTable.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace engine {
namespace registry {
namespace {

bool finite(double v) {
    return std::isfinite(v);
}

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

bool number_field(const sdk::JsonValue& obj, const char* key, double& out,
                  bool required, std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        if (required) {
            errorOut = std::string("missing field ") + key;
            return false;
        }
        return true;
    }
    if (f->kind != sdk::JsonValue::Kind::Number) {
        errorOut = std::string(key) + " must be a number";
        return false;
    }
    out = f->number;
    return true;
}

bool string_field(const sdk::JsonValue& obj, const char* key, std::string& out,
                  bool required, std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        if (required) {
            errorOut = std::string("missing field ") + key;
            return false;
        }
        return true;
    }
    if (f->kind != sdk::JsonValue::Kind::String) {
        errorOut = std::string(key) + " must be a string";
        return false;
    }
    out = f->string;
    return true;
}

bool int_field(const sdk::JsonValue& obj, const char* key, int& out,
               bool required, std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        if (required) {
            errorOut = std::string("missing field ") + key;
            return false;
        }
        return true;
    }
    if (f->kind != sdk::JsonValue::Kind::Number) {
        errorOut = std::string(key) + " must be a number";
        return false;
    }
    const double v = f->number;
    if (v < -2147483648.0 || v > 2147483647.0 || v != std::floor(v)) {
        errorOut = std::string(key) + " must be a whole number";
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

// splitmix64: RNG inteiro puro — determinístico em qualquer plataforma.
std::uint64_t splitmix64(std::uint64_t& state) {
    std::uint64_t z = (state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

}  // namespace

bool LootTableSpec::validate(std::string& errorOut) const {
    if (id.empty()) {
        errorOut = "loot table id must be non-empty";
        return false;
    }
    if (!finite(rolls_min) || rolls_min < 0 ||
        !finite(rolls_max) || rolls_max < rolls_min) {
        errorOut = "rolls_min/rolls_max must be >= 0 with rolls_max >= rolls_min";
        return false;
    }
    for (const auto& e : entries) {
        if (e.item.empty()) {
            errorOut = "entry item must be non-empty";
            return false;
        }
        if (!finite(e.weight) || e.weight <= 0.0) {
            errorOut = "entry weight must be finite and > 0";
            return false;
        }
        if (e.count_min < 0 || e.count_max < e.count_min) {
            errorOut = "entry count range must be >= 0 with count_max >= count_min";
            return false;
        }
        if (!finite(e.chance) || e.chance < 0.0 || e.chance > 1.0) {
            errorOut = "entry chance must be finite and in [0,1]";
            return false;
        }
    }
    errorOut.clear();
    return true;
}

bool LootTableSpec::load_from_json(const std::string& json,
                                   std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(json, doc, errorOut)) {
        return false;
    }
    if (!doc.is_object()) {
        errorOut = "loot table spec must be an object";
        return false;
    }
    const sdk::JsonValue* version = doc.field("version");
    if (version != nullptr &&
        (!is_uint64(*version) || static_cast<int>(version->number) != 1)) {
        errorOut = "unsupported loot table spec version";
        return false;
    }
    LootTableSpec candidate;
    if (!string_field(doc, "id", candidate.id, true, errorOut)) return false;
    if (!int_field(doc, "rolls_min", candidate.rolls_min, false, errorOut))
        return false;
    if (!int_field(doc, "rolls_max", candidate.rolls_max, false, errorOut))
        return false;
    const sdk::JsonValue* entriesField = doc.field("entries");
    if (entriesField != nullptr) {
        if (!entriesField->is_array()) {
            errorOut = "entries must be an array";
            return false;
        }
        for (const auto& item : entriesField->array) {
            if (!item.is_object()) {
                errorOut = "entry entries must be objects";
                return false;
            }
            LootEntry e;
            if (!string_field(item, "item", e.item, true, errorOut)) return false;
            if (!number_field(item, "weight", e.weight, false, errorOut))
                return false;
            if (!int_field(item, "count_min", e.count_min, false, errorOut))
                return false;
            if (!int_field(item, "count_max", e.count_max, false, errorOut))
                return false;
            if (!number_field(item, "chance", e.chance, false, errorOut))
                return false;
            candidate.entries.push_back(e);
        }
    }
    if (!candidate.validate(errorOut)) {
        return false;
    }
    *this = std::move(candidate);
    return true;
}

std::string LootTableSpec::to_json() const {
    std::ostringstream out;
    out << std::setprecision(9);
    out << "{\"version\":1,\"id\":\"" << json_escape(id) << "\",\"rolls_min\":"
        << rolls_min << ",\"rolls_max\":" << rolls_max << ",\"entries\":[";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i) out << ",";
        const auto& e = entries[i];
        out << "{\"item\":\"" << json_escape(e.item) << "\",\"weight\":"
            << e.weight << ",\"count_min\":" << e.count_min << ",\"count_max\":"
            << e.count_max << ",\"chance\":" << e.chance << "}";
    }
    out << "]}";
    return out.str();
}

namespace {

class LootTable final : public ILootTable {
public:
    bool configure(const LootTableSpec& spec, std::string& errorOut) override {
        if (!spec.validate(errorOut)) {
            return false;
        }
        spec_ = spec;
        errorOut.clear();
        return true;
    }

    std::vector<LootRoll> roll(std::uint64_t seed) override {
        std::uint64_t state = seed;
        // 1) número de rolls (rolls_min..rolls_max).
        const int span = spec_.rolls_max - spec_.rolls_min;
        const int num_rolls =
            spec_.rolls_min + static_cast<int>(splitmix64(state) % (span + 1));

        // 2) peso total para a seleção ponderada.
        double total_weight = 0.0;
        for (const auto& e : spec_.entries) {
            total_weight += e.weight;
        }

        std::map<std::string, int> counts;  // merge por item (ordem sorted)
        for (int i = 0; i < num_rolls; ++i) {
            // seleção ponderada (double determinístico em IEEE-754)
            const double r = (static_cast<double>(splitmix64(state) %
                                                  UINT64_C(1000000000)) /
                              1.0e9) *
                             total_weight;
            double acc = 0.0;
            const LootEntry* pick = &spec_.entries.front();
            for (const auto& e : spec_.entries) {
                acc += e.weight;
                if (r < acc) {
                    pick = &e;
                    break;
                }
            }
            // chance por roll
            if (pick->chance < 1.0) {
                const std::uint64_t threshold =
                    static_cast<std::uint64_t>(pick->chance * 1000000.0);
                if ((splitmix64(state) % UINT64_C(1000000)) >= threshold) {
                    continue;  // roll perdido
                }
            }
            // count no range
            const int cspan = pick->count_max - pick->count_min;
            const int count =
                pick->count_min + static_cast<int>(splitmix64(state) % (cspan + 1));
            counts[pick->item] += count;
        }

        // 3) saída mergeada e ordenada por id (iteração de std::map).
        std::vector<LootRoll> out;
        out.reserve(counts.size());
        for (const auto& kv : counts) {
            out.push_back(LootRoll{kv.first, kv.second});
        }
        return out;
    }

    std::vector<std::string> validate_items(
        const std::vector<std::string>& known) const override {
        std::set<std::string> catalog(known.begin(), known.end());
        std::set<std::string> missing;
        for (const auto& e : spec_.entries) {
            if (!catalog.count(e.item)) {
                missing.insert(e.item);
            }
        }
        return std::vector<std::string>(missing.begin(), missing.end());
    }

    std::vector<std::string> items() const override {
        std::set<std::string> ids;
        for (const auto& e : spec_.entries) {
            ids.insert(e.item);
        }
        return std::vector<std::string>(ids.begin(), ids.end());
    }

    std::string serialize_state() const override {
        return spec_.to_json();
    }

    bool deserialize_state(const std::string& json,
                           std::string& errorOut) override {
        LootTableSpec candidate;
        if (!candidate.load_from_json(json, errorOut)) {
            return false;
        }
        if (!candidate.validate(errorOut)) {
            return false;
        }
        spec_ = std::move(candidate);
        errorOut.clear();
        return true;
    }

private:
    LootTableSpec spec_;
};

}  // namespace

std::unique_ptr<ILootTable> create_loot_table() {
    return std::make_unique<LootTable>();
}

}  // namespace registry
}  // namespace engine
