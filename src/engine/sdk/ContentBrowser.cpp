// ContentBrowser.cpp — the ONLY TU with the content-browser behavior (agente
// 2 §B). Pure and deterministic: an asset index + folder tree + search/filter
// + selection for the editor's Content Browser panel. No window/GPU/clock.
// JSON via RegistryJson.

#include "engine/editor/IContentBrowser.hpp"

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

bool asset_less(const ContentAsset& a, const ContentAsset& b) {
    if (a.folder != b.folder) return a.folder < b.folder;
    return a.name < b.name;
}

}  // namespace

bool ContentBrowserDoc::validate(std::string& errorOut) const {
    errorOut.clear();
    if (version != 1) {
        errorOut = "unsupported content browser version";
        return false;
    }
    std::map<std::string, bool> seen;
    for (const ContentAsset& asset : assets) {
        if (asset.id.empty()) {
            errorOut = "content asset id must not be empty";
            return false;
        }
        if (asset.name.empty()) {
            errorOut = "content asset name must not be empty";
            return false;
        }
        if (seen.count(asset.id)) {
            errorOut = "duplicate content asset id: " + asset.id;
            return false;
        }
        seen[asset.id] = true;
    }
    return true;
}

std::string ContentBrowserDoc::to_json() const {
    std::ostringstream out;
    out << "{\"version\":" << version << ",\"assets\":[";
    for (std::size_t i = 0; i < assets.size(); ++i) {
        if (i) out << ',';
        out << "{\"id\":\"" << json_escape(assets[i].id)
            << "\",\"name\":\"" << json_escape(assets[i].name)
            << "\",\"type\":\"" << json_escape(assets[i].type)
            << "\",\"folder\":\"" << json_escape(assets[i].folder) << "\"}";
    }
    out << "]}";
    return out.str();
}

bool ContentBrowserDoc::load_from_json(const std::string& jsonText,
                                       std::string& errorOut) {
    sdk::JsonValue doc;
    if (!sdk::json_parse(jsonText, doc, errorOut)) return false;
    if (!doc.is_object()) {
        errorOut = "content browser document must be an object";
        return false;
    }
    const int version = static_cast<int>(sdk::json_number(doc, "version", 1));
    if (version != 1) {
        errorOut = "unsupported content browser version";
        return false;
    }
    ContentBrowserDoc candidate;
    candidate.version = version;
    if (const sdk::JsonValue* arr = doc.field("assets")) {
        if (!arr->is_array()) {
            errorOut = "content browser field 'assets' must be an array";
            return false;
        }
        for (const sdk::JsonValue& entry : arr->array) {
            ContentAsset asset;
            asset.id = sdk::json_string(entry, "id", "");
            asset.name = sdk::json_string(entry, "name", "");
            asset.type = sdk::json_string(entry, "type", "");
            asset.folder = sdk::json_string(entry, "folder", "");
            candidate.assets.push_back(std::move(asset));
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

class ContentBrowserRuntime final : public IContentBrowser {
public:
    explicit ContentBrowserRuntime(const ContentBrowserDoc& doc) : doc_(doc) {
        for (const ContentAsset& asset : doc_.assets) {
            byId_[asset.id] = asset;
        }
    }

    std::vector<ContentAsset> assets() const override {
        std::vector<ContentAsset> out = doc_.assets;
        std::stable_sort(out.begin(), out.end(), asset_less);
        return out;
    }

    std::vector<ContentFolder> folders() const override {
        // Group by folder; emit only folders with direct assets, sorted.
        std::map<std::string, std::vector<ContentAsset>> groups;
        for (const ContentAsset& asset : doc_.assets) {
            groups[asset.folder].push_back(asset);
        }
        std::vector<ContentFolder> out;
        for (auto& entry : groups) {
            ContentFolder folder;
            folder.path = entry.first;
            std::stable_sort(entry.second.begin(), entry.second.end(),
                             [](const ContentAsset& a, const ContentAsset& b) {
                return a.name < b.name;
            });
            folder.assets = std::move(entry.second);
            out.push_back(std::move(folder));
        }
        return out;
    }

    std::vector<ContentAsset> search(const std::string& query) const override {
        const std::string q = lower(query);
        std::vector<ContentAsset> out;
        if (q.empty()) return assets();
        for (const ContentAsset& asset : doc_.assets) {
            if (lower(asset.name).find(q) != std::string::npos ||
                lower(asset.folder).find(q) != std::string::npos) {
                out.push_back(asset);
            }
        }
        std::stable_sort(out.begin(), out.end(), asset_less);
        return out;
    }

    std::vector<ContentAsset> by_type(const std::string& type) const override {
        std::vector<ContentAsset> out;
        for (const ContentAsset& asset : doc_.assets) {
            if (asset.type == type) out.push_back(asset);
        }
        std::stable_sort(out.begin(), out.end(), asset_less);
        return out;
    }

    const ContentAsset* find(const std::string& id) const override {
        const auto it = byId_.find(id);
        return it == byId_.end() ? nullptr : &it->second;
    }

    bool select(const std::string& id) override {
        if (!byId_.count(id)) return false;
        selectionId_ = id;
        return true;
    }

    void clear_selection() override { selectionId_.clear(); }

    bool has_selection() const override { return !selectionId_.empty(); }

    std::string selection_id() const override { return selectionId_; }

    bool add(const ContentAsset& asset, std::string& errorOut) override {
        errorOut.clear();
        if (asset.id.empty()) {
            errorOut = "content asset id must not be empty";
            return false;
        }
        if (asset.name.empty()) {
            errorOut = "content asset name must not be empty";
            return false;
        }
        if (byId_.count(asset.id)) {
            errorOut = "duplicate content asset id: " + asset.id;
            return false;
        }
        doc_.assets.push_back(asset);
        byId_[asset.id] = asset;
        return true;
    }

    std::size_t size() const override { return byId_.size(); }

    const ContentBrowserDoc& spec() const override { return doc_; }

    std::string to_json() const override {
        std::ostringstream out;
        out << "{\"version\":" << doc_.version << ",\"assets\":[";
        const auto sorted = assets();
        for (std::size_t i = 0; i < sorted.size(); ++i) {
            if (i) out << ',';
            out << "{\"id\":\"" << json_escape(sorted[i].id)
                << "\",\"name\":\"" << json_escape(sorted[i].name)
                << "\",\"type\":\"" << json_escape(sorted[i].type)
                << "\",\"folder\":\"" << json_escape(sorted[i].folder) << "\"}";
        }
        out << "],\"folders\":[";
        const auto tree = folders();
        for (std::size_t i = 0; i < tree.size(); ++i) {
            if (i) out << ',';
            out << "{\"path\":\"" << json_escape(tree[i].path)
                << "\",\"count\":" << tree[i].assets.size() << "}";
        }
        out << "],\"selection\":\"" << json_escape(selectionId_) << "\"}";
        return out.str();
    }

private:
    ContentBrowserDoc doc_;
    std::map<std::string, ContentAsset> byId_;
    std::string selectionId_;
};

}  // namespace

std::unique_ptr<IContentBrowser> create_content_browser(
    const ContentBrowserDoc& doc, std::string& errorOut) {
    errorOut.clear();
    if (!doc.validate(errorOut)) return nullptr;
    return std::make_unique<ContentBrowserRuntime>(doc);
}

}  // namespace editor
}  // namespace engine
