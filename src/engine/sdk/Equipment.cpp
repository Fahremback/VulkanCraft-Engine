#include "engine/registry/IEquipment.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace engine {
namespace registry {
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

bool string_array_field(const sdk::JsonValue& obj, const char* key,
                        std::vector<std::string>& out, std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        return true;
    }
    if (!f->is_array()) {
        errorOut = std::string(key) + " must be an array";
        return false;
    }
    out.clear();
    for (const auto& item : f->array) {
        if (item.kind != sdk::JsonValue::Kind::String) {
            errorOut = std::string(key) + " entries must be strings";
            return false;
        }
        out.push_back(item.string);
    }
    return true;
}

}  // namespace

bool EquipmentSpec::validate(std::string& errorOut) const {
    std::set<std::string> ids;
    for (const auto& c : categories) {
        if (c.id.empty()) {
            errorOut = "category id must be non-empty";
            return false;
        }
        if (ids.count(c.id)) {
            errorOut = "duplicate category id \"" + c.id + "\"";
            return false;
        }
        ids.insert(c.id);
        std::set<std::string> seen;
        for (const auto& t : c.tags) {
            if (t.empty()) {
                errorOut = "category tag must be non-empty";
                return false;
            }
            if (seen.count(t)) {
                errorOut = "category \"" + c.id + "\" duplicates tag \"" + t + "\"";
                return false;
            }
            seen.insert(t);
        }
    }
    errorOut.clear();
    return true;
}

bool EquipmentSpec::load_from_json(const std::string& json,
                                   std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(json, doc, errorOut)) {
        return false;
    }
    if (!doc.is_object()) {
        errorOut = "equipment spec must be an object";
        return false;
    }
    const sdk::JsonValue* version = doc.field("version");
    if (version != nullptr &&
        (!is_uint64(*version) || static_cast<int>(version->number) != 1)) {
        errorOut = "unsupported equipment spec version";
        return false;
    }
    EquipmentSpec candidate;
    const sdk::JsonValue* categoriesField = doc.field("categories");
    if (categoriesField != nullptr) {
        if (!categoriesField->is_array()) {
            errorOut = "categories must be an array";
            return false;
        }
        for (const auto& item : categoriesField->array) {
            if (!item.is_object()) {
                errorOut = "category entries must be objects";
                return false;
            }
            EquipmentCategory c;
            if (!string_field(item, "id", c.id, true, errorOut)) return false;
            if (!string_array_field(item, "tags", c.tags, errorOut)) return false;
            candidate.categories.push_back(c);
        }
    }
    if (!candidate.validate(errorOut)) {
        return false;
    }
    *this = std::move(candidate);
    return true;
}

std::string EquipmentSpec::to_json() const {
    std::ostringstream out;
    out << "{\"version\":1,\"categories\":[";
    for (std::size_t i = 0; i < categories.size(); ++i) {
        if (i) out << ",";
        const auto& c = categories[i];
        out << "{\"id\":\"" << json_escape(c.id) << "\",\"tags\":[";
        for (std::size_t j = 0; j < c.tags.size(); ++j) {
            if (j) out << ",";
            out << "\"" << json_escape(c.tags[j]) << "\"";
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

namespace {

class Equipment final : public IEquipment {
public:
    bool configure(const EquipmentSpec& spec, std::string& errorOut) override {
        if (!spec.validate(errorOut)) {
            return false;
        }
        spec_ = spec;
        slots_.clear();
        for (const auto& c : spec_.categories) {
            slots_[c.id] = "";
        }
        errorOut.clear();
        return true;
    }

    bool equip(const std::string& category, const std::string& item,
               const std::vector<std::string>& itemTags,
               std::string& errorOut) override {
        if (item.empty()) {
            errorOut = "item must be non-empty";
            return false;
        }
        const EquipmentCategory* cat = find_category(category);
        if (cat == nullptr) {
            errorOut = "unknown category \"" + category + "\"";
            return false;
        }
        if (!cat->tags.empty()) {
            std::set<std::string> allowed(cat->tags.begin(), cat->tags.end());
            bool fits = false;
            for (const auto& t : itemTags) {
                if (allowed.count(t)) {
                    fits = true;
                    break;
                }
            }
            if (!fits) {
                errorOut = "item \"" + item + "\" does not fit category \"" +
                           category + "\"";
                return false;
            }
        }
        slots_[category] = item;
        errorOut.clear();
        return true;
    }

    bool unequip(const std::string& category, std::string& errorOut) override {
        if (!slots_.count(category)) {
            errorOut = "unknown category \"" + category + "\"";
            return false;
        }
        slots_[category] = "";
        errorOut.clear();
        return true;
    }

    std::string equipped(const std::string& category,
                         std::string& errorOut) const override {
        const auto it = slots_.find(category);
        if (it == slots_.end()) {
            errorOut = "unknown category \"" + category + "\"";
            return "";
        }
        errorOut.clear();
        return it->second;
    }

    std::vector<std::pair<std::string, std::string>> items() const override {
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& kv : slots_) {
            if (!kv.second.empty()) {
                out.emplace_back(kv.first, kv.second);
            }
        }
        return out;
    }

    std::vector<std::string> categories() const override {
        std::vector<std::string> out;
        for (const auto& c : spec_.categories) {
            out.push_back(c.id);
        }
        return out;
    }

    std::string serialize_state() const override {
        std::ostringstream out;
        out << "{\"slots\":{";
        bool first = true;
        for (const auto& kv : slots_) {
            if (!first) out << ",";
            first = false;
            out << "\"" << json_escape(kv.first) << "\":\""
                << json_escape(kv.second) << "\"";
        }
        out << "}}";
        return out.str();
    }

    bool deserialize_state(const std::string& json,
                           std::string& errorOut) override {
        sdk::JsonValue doc;
        if (!sdk::json_parse(json, doc, errorOut)) {
            return false;
        }
        if (!doc.is_object()) {
            errorOut = "equipment state must be an object";
            return false;
        }
        const sdk::JsonValue* slotsField = doc.field("slots");
        if (slotsField == nullptr || !slotsField->is_object()) {
            errorOut = "state must contain a slots object";
            return false;
        }
        // all-or-nothing: valida antes de mutar.
        std::map<std::string, std::string> nslots;
        for (const auto& kv : slotsField->object) {
            if (!slots_.count(kv.first)) {
                errorOut = "state references unknown category \"" + kv.first + "\"";
                return false;
            }
            if (kv.second.kind != sdk::JsonValue::Kind::String) {
                errorOut = "slot values must be strings";
                return false;
            }
            nslots[kv.first] = kv.second.string;
        }
        slots_ = nslots;
        errorOut.clear();
        return true;
    }

private:
    const EquipmentCategory* find_category(const std::string& id) const {
        for (const auto& c : spec_.categories) {
            if (c.id == id) {
                return &c;
            }
        }
        return nullptr;
    }

    EquipmentSpec spec_;
    std::map<std::string, std::string> slots_;
};

}  // namespace

std::unique_ptr<IEquipment> create_equipment() {
    return std::make_unique<Equipment>();
}

}  // namespace registry
}  // namespace engine
