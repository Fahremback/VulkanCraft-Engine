#include "engine/ai/IAiEventBus.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace engine {
namespace ai {
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

bool int_field(const sdk::JsonValue& obj, const char* key, int& out,
               std::string& errorOut) {
    const sdk::JsonValue* f = obj.field(key);
    if (f == nullptr) {
        return true;
    }
    if (!is_uint64(*f)) {
        errorOut = std::string(key) + " must be a non-negative integer";
        return false;
    }
    out = static_cast<int>(f->number);
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

}  // namespace

bool AiEventBusSpec::validate(std::string& errorOut) const {
    if (max_events < 0) {
        errorOut = "max_events must be >= 0";
        return false;
    }
    errorOut.clear();
    return true;
}

bool AiEventBusSpec::load_from_json(const std::string& json, std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(json, doc, errorOut)) {
        return false;
    }
    if (!doc.is_object()) {
        errorOut = "event bus spec must be an object";
        return false;
    }
    AiEventBusSpec candidate;
    if (!int_field(doc, "max_events", candidate.max_events, errorOut)) {
        return false;
    }
    if (!candidate.validate(errorOut)) {
        return false;
    }
    *this = candidate;
    return true;
}

std::string AiEventBusSpec::to_json() const {
    std::ostringstream out;
    out << "{\"max_events\":" << max_events << "}";
    return out.str();
}

namespace {

class AiEventBus final : public IAiEventBus {
public:
    bool configure(const AiEventBusSpec& spec, std::string& errorOut) override {
        if (!spec.validate(errorOut)) {
            return false;
        }
        spec_ = spec;
        events_.clear();
        return true;
    }

    void emit(std::uint64_t tick, const std::string& source,
              const std::string& kind, const std::string& payload) override {
        events_.push_back(AiEvent{tick, source, kind, payload});
        if (spec_.max_events > 0 &&
            static_cast<int>(events_.size()) > spec_.max_events) {
            const int excess = static_cast<int>(events_.size()) - spec_.max_events;
            events_.erase(events_.begin(), events_.begin() + excess);  // FIFO
        }
    }

    std::vector<AiEvent> peek() const override { return events_; }

    std::vector<AiEvent> drain() override {
        std::vector<AiEvent> out = std::move(events_);
        events_.clear();
        return out;
    }

    void clear() override { events_.clear(); }

    std::string serialize() const override {
        std::ostringstream out;
        out << "{\"events\":[";
        for (std::size_t i = 0; i < events_.size(); ++i) {
            if (i) out << ",";
            const auto& e = events_[i];
            out << "{\"tick\":" << e.tick << ",\"source\":\"" << json_escape(e.source)
                << "\",\"kind\":\"" << json_escape(e.kind) << "\",\"payload\":\""
                << json_escape(e.payload) << "\"}";
        }
        out << "]}";
        return out.str();
    }

    bool deserialize(const std::string& json, std::string& errorOut) override {
        sdk::JsonValue doc;
        if (!sdk::json_parse(json, doc, errorOut)) {
            return false;
        }
        if (!doc.is_object()) {
            errorOut = "event bus state must be an object";
            return false;
        }
        const sdk::JsonValue* eventsField = doc.field("events");
        if (eventsField == nullptr || !eventsField->is_array()) {
            errorOut = "state must contain an events array";
            return false;
        }
        std::vector<AiEvent> candidate;
        for (const auto& item : eventsField->array) {
            if (!item.is_object()) {
                errorOut = "event entries must be objects";
                return false;
            }
            AiEvent e;
            const sdk::JsonValue* tick = item.field("tick");
            if (tick == nullptr || !is_uint64(*tick)) {
                errorOut = "event tick must be a non-negative integer";
                return false;
            }
            e.tick = static_cast<std::uint64_t>(tick->number);
            if (!string_field(item, "source", e.source, true, errorOut)) return false;
            if (!string_field(item, "kind", e.kind, true, errorOut)) return false;
            if (!string_field(item, "payload", e.payload, false, errorOut)) return false;
            candidate.push_back(e);
        }
        events_ = std::move(candidate);
        errorOut.clear();
        return true;
    }

private:
    AiEventBusSpec spec_;
    std::vector<AiEvent> events_;
};

}  // namespace

std::unique_ptr<IAiEventBus> create_ai_event_bus() {
    return std::make_unique<AiEventBus>();
}

}  // namespace ai
}  // namespace engine
