#pragma once

// IContentBrowser (agente 2 §B): the PUBLIC model of the editor's Content
// Browser panel. Today the panel is a visual ImGui tree fed ad-hoc from the
// asset registry with NO deterministic navigation model — this contract makes
// browsing data-driven and bit-exact testable:
//   - ASSET INDEX: {id, name, type, folder} entries, versioned JSON
//     round-trippable (all-or-nothing on load: empty/duplicate id refused).
//   - FOLDERS: derived deterministically from asset paths — a stable tree of
//     unique folders (sorted), each listing its direct assets (sorted by
//     name). Folders with no assets are never emitted.
//   - FILTER: search(query) matches name AND folder (case-insensitive
//     substring), and by_type(type) filters to one type. Results sorted by
//     (folder, name) — bit-exact for the same index.
//   - SELECTION: select(id) / clear_selection() — one selected asset at a
//     time (selecting an unknown id refused, no mutation); selection()
//     returns the selected asset or none. Selection is part of the JSON
//     snapshot so the panel state is observable.
//   - DETERMINISM: no clocks/RNG/globals; same index + queries -> identical
//     results and JSON.
//
// Self-contained (std only). The SDK adapter
// (src/engine/sdk/ContentBrowser.cpp) is the ONLY TU with behavior.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace editor {

// One asset shown in the browser.
struct ContentAsset {
    std::string id;     // stable id (required, unique)
    std::string name;   // display name (required)
    std::string type;   // "texture" | "model" | "audio" | "scene" | "script" | ...
    std::string folder; // folder path ("" = root); derived from the asset path

    bool operator==(const ContentAsset& other) const {
        return id == other.id && name == other.name &&
               type == other.type && folder == other.folder;
    }
    bool operator!=(const ContentAsset& other) const {
        return !(*this == other);
    }
};

// One folder in the tree, with its direct assets.
struct ContentFolder {
    std::string path;                  // folder path ("" = root)
    std::vector<ContentAsset> assets;  // direct children, sorted by name

    bool operator==(const ContentFolder& other) const {
        return path == other.path && assets == other.assets;
    }
    bool operator!=(const ContentFolder& other) const {
        return !(*this == other);
    }
};

// The content index document, versioned and JSON round-trippable.
struct ContentBrowserDoc {
    int version{ 1 };
    std::vector<ContentAsset> assets;

    bool validate(std::string& errorOut) const;
    bool load_from_json(const std::string& jsonText, std::string& errorOut);
    std::string to_json() const;
};

class IContentBrowser {
public:
    virtual ~IContentBrowser() = default;

    // All assets, sorted by (folder, name) — deterministic.
    virtual std::vector<ContentAsset> assets() const = 0;

    // Unique folders (sorted); each lists its DIRECT assets (sorted). Empty
    // folders are never emitted.
    virtual std::vector<ContentFolder> folders() const = 0;

    // Case-insensitive substring search over name and folder. Sorted by
    // (folder, name). Empty query -> all assets.
    virtual std::vector<ContentAsset> search(const std::string& query) const = 0;

    // Assets of one type (exact match). Sorted by (folder, name).
    virtual std::vector<ContentAsset> by_type(const std::string& type) const = 0;

    // Looks up one asset by id (nullptr when unknown).
    virtual const ContentAsset* find(const std::string& id) const = 0;

    // Selection: one asset at a time. select() on an unknown id is refused
    // (false, selection unchanged). clear_selection() clears.
    virtual bool select(const std::string& id) = 0;
    virtual void clear_selection() = 0;
    virtual bool has_selection() const = 0;
    virtual std::string selection_id() const = 0;

    // Adds an asset at runtime (all-or-nothing: empty/duplicate id refused).
    virtual bool add(const ContentAsset& asset, std::string& errorOut) = 0;

    virtual std::size_t size() const = 0;

    virtual const ContentBrowserDoc& spec() const = 0;

    // Deterministic JSON snapshot ({"version":1,"assets":[...],
    // "folders":[...],"selection":"..."}).
    virtual std::string to_json() const = 0;
};

// Parses+validates a content index and compiles it (rejected -> nullptr +
// errorOut).
std::unique_ptr<IContentBrowser> create_content_browser(
    const ContentBrowserDoc& doc, std::string& errorOut);

}  // namespace editor
}  // namespace engine
