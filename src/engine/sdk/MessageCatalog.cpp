// MessageCatalog.cpp — the ONLY TU with the message-catalog behavior (agente
// 2 §C). Pure and deterministic: a curated catalog of user-facing messages
// with stable ids, severity, parameterized text ({0}..{n}) and an actionable
// hint. render() always succeeds (unknown id or missing params -> visible
// marker + diagnostic, never a crash). No window/GPU. JSON via RegistryJson.

#include "engine/editor/IMessageCatalog.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <map>
#include <sstream>

namespace engine {
namespace editor {

namespace {

const char* severity_name(MessageSeverity severity) {
    switch (severity) {
        case MessageSeverity::Info: return "info";
        case MessageSeverity::Warning: return "warning";
        case MessageSeverity::Error: break;
    }
    return "error";
}

bool severity_from_name(const std::string& name, MessageSeverity& out) {
    if (name == "info") { out = MessageSeverity::Info; return true; }
    if (name == "warning") { out = MessageSeverity::Warning; return true; }
    if (name == "error") { out = MessageSeverity::Error; return true; }
    return false;
}

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

bool MessageCatalogDoc::validate(std::string& errorOut) const {
    errorOut.clear();
    if (version != 1) {
        errorOut = "unsupported message catalog version";
        return false;
    }
    std::map<std::string, bool> seen;
    for (const CatalogMessage& message : messages) {
        if (message.id.empty()) {
            errorOut = "catalog message id must not be empty";
            return false;
        }
        if (message.text.empty()) {
            errorOut = "catalog message text must not be empty";
            return false;
        }
        if (seen.count(message.id)) {
            errorOut = "duplicate catalog message id: " + message.id;
            return false;
        }
        seen[message.id] = true;
    }
    return true;
}

std::string MessageCatalogDoc::to_json() const {
    std::ostringstream out;
    out << "{\"version\":" << version << ",\"messages\":[";
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (i) out << ',';
        out << "{\"id\":\"" << json_escape(messages[i].id)
            << "\",\"severity\":\"" << severity_name(messages[i].severity)
            << "\",\"text\":\"" << json_escape(messages[i].text)
            << "\",\"action\":\"" << json_escape(messages[i].action) << "\"}";
    }
    out << "]}";
    return out.str();
}

bool MessageCatalogDoc::load_from_json(const std::string& jsonText,
                                       std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(jsonText, doc, errorOut)) return false;
    if (!doc.is_object()) {
        errorOut = "message catalog document must be an object";
        return false;
    }
    const int version = static_cast<int>(sdk::json_number(doc, "version", 1));
    if (version != 1) {
        errorOut = "unsupported message catalog version";
        return false;
    }
    MessageCatalogDoc candidate;
    candidate.version = version;
    if (const sdk::JsonValue* arr = doc.field("messages")) {
        if (!arr->is_array()) {
            errorOut = "message catalog field 'messages' must be an array";
            return false;
        }
        for (const sdk::JsonValue& entry : arr->array) {
            CatalogMessage message;
            message.id = sdk::json_string(entry, "id", "");
            message.text = sdk::json_string(entry, "text", "");
            message.action = sdk::json_string(entry, "action", "");
            const std::string severity =
                sdk::json_string(entry, "severity", "info");
            if (!severity_from_name(severity, message.severity)) {
                errorOut = "unknown catalog severity: " + severity;
                return false;
            }
            candidate.messages.push_back(std::move(message));
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

class MessageCatalogRuntime final : public IMessageCatalog {
public:
    explicit MessageCatalogRuntime(const MessageCatalogDoc& doc) : doc_(doc) {
        for (const CatalogMessage& message : doc_.messages) {
            byId_[message.id] = message;
        }
    }

    const CatalogMessage* find(const std::string& id) const override {
        const auto it = byId_.find(id);
        return it == byId_.end() ? nullptr : &it->second;
    }

    std::string render(const std::string& id,
                       const std::vector<std::string>& params,
                       std::string& errorOut) const override {
        errorOut.clear();
        const CatalogMessage* message = find(id);
        if (message == nullptr) {
            errorOut = "unknown message id: " + id;
            return "[unknown:" + id + "]";
        }
        // Fill {0}..{n} placeholders; missing params stay visible.
        std::string out;
        out.reserve(message->text.size() + 16);
        for (std::size_t i = 0; i < message->text.size(); ++i) {
            const char c = message->text[i];
            // A placeholder is exactly {d} (single digit + closing brace).
            if (c == '{' && i + 2 < message->text.size() &&
                message->text[i + 1] >= '0' && message->text[i + 1] <= '9' &&
                message->text[i + 2] == '}') {
                const std::size_t index =
                    static_cast<std::size_t>(message->text[i + 1] - '0');
                if (index < params.size()) {
                    out += params[index];
                } else {
                    out += '{';
                    out += message->text[i + 1];
                    out += '}';
                    if (errorOut.empty()) {
                        errorOut = "missing parameter {" +
                                   std::string(1, message->text[i + 1]) +
                                   "} for message " + id;
                    }
                }
                i += 2;  // consume '{d}' (loop's ++ moves past '}')
                continue;
            }
            out += c;
        }
        return out;
    }

    std::vector<std::string> ids() const override {
        std::vector<std::string> out;
        out.reserve(byId_.size());
        for (const auto& entry : byId_) out.push_back(entry.first);
        std::sort(out.begin(), out.end());
        return out;
    }

    bool add(const CatalogMessage& message, std::string& errorOut) override {
        errorOut.clear();
        if (message.id.empty()) {
            errorOut = "catalog message id must not be empty";
            return false;
        }
        if (message.text.empty()) {
            errorOut = "catalog message text must not be empty";
            return false;
        }
        if (byId_.count(message.id)) {
            errorOut = "duplicate catalog message id: " + message.id;
            return false;
        }
        doc_.messages.push_back(message);
        byId_[message.id] = message;
        return true;
    }

    std::size_t size() const override { return byId_.size(); }

    const MessageCatalogDoc& spec() const override { return doc_; }

private:
    MessageCatalogDoc doc_;
    std::map<std::string, CatalogMessage> byId_;
};

}  // namespace

std::unique_ptr<IMessageCatalog> create_message_catalog(
    const MessageCatalogDoc& doc, std::string& errorOut) {
    errorOut.clear();
    if (!doc.validate(errorOut)) return nullptr;
    return std::make_unique<MessageCatalogRuntime>(doc);
}

}  // namespace editor
}  // namespace engine
