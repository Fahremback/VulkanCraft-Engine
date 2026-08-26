// CommandSearch.cpp — the ONLY TU with the command-search behavior (agente 2
// §B). Pure and deterministic: a data-driven command index + ranked search
// (the command-palette / global-search backbone). No window/GPU/clock/RNG.
// JSON via RegistryJson.

#include "engine/editor/ICommandSearch.hpp"

#include "RegistryJson.hpp"

#include <algorithm>
#include <cctype>
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

std::string lower(const std::string& value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Whether `text` contains `part` at a word boundary (start, or preceded by a
// non-alphanumeric char).
bool word_boundary_contains(const std::string& text, const std::string& part) {
    const std::size_t pos = text.find(part);
    if (pos == std::string::npos) return false;
    if (pos == 0) return true;
    const char prev = text[pos - 1];
    return !std::isalnum(static_cast<unsigned char>(prev));
}

}  // namespace

bool CommandIndexDoc::validate(std::string& errorOut) const {
    errorOut.clear();
    if (version != 1) {
        errorOut = "unsupported command index version";
        return false;
    }
    std::map<std::string, bool> seen;
    for (const CommandEntry& entry : entries) {
        if (entry.id.empty()) {
            errorOut = "command entry id must not be empty";
            return false;
        }
        if (entry.label.empty()) {
            errorOut = "command entry label must not be empty";
            return false;
        }
        if (seen.count(entry.id)) {
            errorOut = "duplicate command entry id: " + entry.id;
            return false;
        }
        seen[entry.id] = true;
    }
    return true;
}

std::string CommandIndexDoc::to_json() const {
    std::ostringstream out;
    out << "{\"version\":" << version << ",\"entries\":[";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i) out << ',';
        out << "{\"id\":\"" << json_escape(entries[i].id)
            << "\",\"label\":\"" << json_escape(entries[i].label)
            << "\",\"category\":\"" << json_escape(entries[i].category)
            << "\",\"keywords\":[";
        for (std::size_t k = 0; k < entries[i].keywords.size(); ++k) {
            if (k) out << ',';
            out << '\"' << json_escape(entries[i].keywords[k]) << '\"';
        }
        out << "],\"action\":\"" << json_escape(entries[i].action) << "\"}";
    }
    out << "]}";
    return out.str();
}

bool CommandIndexDoc::load_from_json(const std::string& jsonText,
                                     std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(jsonText, doc, errorOut)) return false;
    if (!doc.is_object()) {
        errorOut = "command index document must be an object";
        return false;
    }
    const int version = static_cast<int>(sdk::json_number(doc, "version", 1));
    if (version != 1) {
        errorOut = "unsupported command index version";
        return false;
    }
    CommandIndexDoc candidate;
    candidate.version = version;
    if (const sdk::JsonValue* arr = doc.field("entries")) {
        if (!arr->is_array()) {
            errorOut = "command index field 'entries' must be an array";
            return false;
        }
        for (const sdk::JsonValue& entry : arr->array) {
            CommandEntry cmd;
            cmd.id = sdk::json_string(entry, "id", "");
            cmd.label = sdk::json_string(entry, "label", "");
            cmd.category = sdk::json_string(entry, "category", "");
            cmd.action = sdk::json_string(entry, "action", "");
            cmd.keywords = sdk::json_string_array(entry, "keywords");
            candidate.entries.push_back(std::move(cmd));
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

class CommandSearchRuntime final : public ICommandSearch {
public:
    explicit CommandSearchRuntime(const CommandIndexDoc& doc) : doc_(doc) {
        for (const CommandEntry& entry : doc_.entries) {
            byId_[entry.id] = entry;
            indexed_.push_back(IndexedEntry{ entry, lower(entry.label),
                                             lower(entry.category), {} });
            IndexedEntry& idx = indexed_.back();
            for (const std::string& kw : entry.keywords) {
                idx.lowerKeywords.push_back(lower(kw));
            }
        }
    }

    std::vector<CommandHit> search(const std::string& query) const override {
        const std::string q = lower(query);
        std::vector<CommandHit> hits;
        if (q.empty()) {
            // Full index in declaration order.
            for (const IndexedEntry& idx : indexed_) {
                CommandHit hit;
                hit.id = idx.entry.id;
                hit.label = idx.entry.label;
                hit.category = idx.entry.category;
                hit.action = idx.entry.action;
                hit.score = 0;
                hits.push_back(std::move(hit));
            }
            return hits;
        }

        for (const IndexedEntry& idx : indexed_) {
            std::int64_t score = 0;
            if (idx.lowerLabel.find(q) == 0) {
                score += 100;  // prefix match on label
            } else if (word_boundary_contains(idx.lowerLabel, q)) {
                score += 60;  // word-boundary match on label
            } else if (idx.lowerLabel.find(q) != std::string::npos) {
                score += 30;  // substring match on label
            }
            if (score == 0) {
                for (const std::string& kw : idx.lowerKeywords) {
                    if (kw.find(q) == 0 || kw.find(q) != std::string::npos) {
                        score += 20;  // keyword prefix or substring
                        break;
                    }
                }
            }
            if (score == 0 && idx.lowerCategory.find(q) == 0) {
                score += 10;  // category prefix match
            }
            if (score == 0) continue;
            CommandHit hit;
            hit.id = idx.entry.id;
            hit.label = idx.entry.label;
            hit.category = idx.entry.category;
            hit.action = idx.entry.action;
            hit.score = score;
            hits.push_back(std::move(hit));
        }

        // Stable deterministic ordering: score DESC, then label-prefix length
        // DESC (longest prefix wins), then (category, id) ASC.
        std::stable_sort(hits.begin(), hits.end(),
                         [&q](const CommandHit& a, const CommandHit& b) {
            if (a.score != b.score) return a.score > b.score;
            const std::string la = lower(a.label);
            const std::string lb = lower(b.label);
            const std::size_t pa = la.find(q);
            const std::size_t pb = lb.find(q);
            const std::size_t lenA = (pa == 0) ? q.size() : 0;
            const std::size_t lenB = (pb == 0) ? q.size() : 0;
            if (lenA != lenB) return lenA > lenB;
            if (a.category != b.category) return a.category < b.category;
            return a.id < b.id;
        });
        return hits;
    }

    const CommandEntry* find(const std::string& id) const override {
        const auto it = byId_.find(id);
        return it == byId_.end() ? nullptr : &it->second;
    }

    bool add(const CommandEntry& entry, std::string& errorOut) override {
        errorOut.clear();
        if (entry.id.empty()) {
            errorOut = "command entry id must not be empty";
            return false;
        }
        if (entry.label.empty()) {
            errorOut = "command entry label must not be empty";
            return false;
        }
        if (byId_.count(entry.id)) {
            errorOut = "duplicate command entry id: " + entry.id;
            return false;
        }
        doc_.entries.push_back(entry);
        byId_[entry.id] = entry;
        IndexedEntry idx;
        idx.entry = entry;
        idx.lowerLabel = lower(entry.label);
        idx.lowerCategory = lower(entry.category);
        for (const std::string& kw : entry.keywords) {
            idx.lowerKeywords.push_back(lower(kw));
        }
        indexed_.push_back(std::move(idx));
        return true;
    }

    std::size_t size() const override { return byId_.size(); }

    const CommandIndexDoc& spec() const override { return doc_; }

private:
    struct IndexedEntry {
        CommandEntry entry;
        std::string lowerLabel;
        std::string lowerCategory;
        std::vector<std::string> lowerKeywords;
    };

    CommandIndexDoc doc_;
    std::map<std::string, CommandEntry> byId_;
    std::vector<IndexedEntry> indexed_;
};

}  // namespace

std::unique_ptr<ICommandSearch> create_command_search(
    const CommandIndexDoc& doc, std::string& errorOut) {
    errorOut.clear();
    if (!doc.validate(errorOut)) return nullptr;
    return std::make_unique<CommandSearchRuntime>(doc);
}

}  // namespace editor
}  // namespace engine
