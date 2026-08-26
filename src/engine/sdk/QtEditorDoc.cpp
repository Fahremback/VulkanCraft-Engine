// IQtEditorDoc adapter (agente 2 §B — porte Qt): deterministic document model
// of the Qt editor shell (docks/actions/menus/toolbars/status). All-or-nothing
// mutations, bit-exact JSON (no floats), std only.

#include "engine/ui/qt/IQtEditorDoc.hpp"

#include <cstddef>
#include <cstdio>
#include <sstream>
#include <unordered_map>

namespace engine {
namespace ui {

const char* qt_dock_area_name(QtDockArea area) {
    switch (area) {
        case QtDockArea::Left: return "Left";
        case QtDockArea::Right: return "Right";
        case QtDockArea::Top: return "Top";
        case QtDockArea::Bottom: return "Bottom";
        case QtDockArea::Floating: return "Floating";
    }
    return "Left";
}

namespace {

// Minimal JSON string escape (the doc carries user-visible titles/labels).
std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

class QtEditorDocImpl final : public IQtEditorDoc {
public:
    QtEditorDocImpl() = default;

    bool set_doc(const QtEditorDocSnapshot& doc, std::string& errorOut) override {
        if (doc.version.empty()) {
            errorOut = "document version must not be empty";
            return false;
        }
        if (doc.docks.empty()) {
            errorOut = "document must declare at least one dock";
            return false;
        }
        if (doc.actions.empty()) {
            errorOut = "document must declare at least one action";
            return false;
        }
        std::unordered_map<std::string, std::size_t> dockIndex;
        dockIndex.reserve(doc.docks.size());
        for (std::size_t i = 0; i < doc.docks.size(); ++i) {
            const auto& d = doc.docks[i];
            if (d.objectName.empty()) {
                errorOut = "dock #" + std::to_string(i) + " has an empty objectName";
                return false;
            }
            if (!dockIndex.emplace(d.objectName, i).second) {
                errorOut = "duplicate dock objectName '" + d.objectName + "'";
                return false;
            }
        }
        std::unordered_map<std::string, std::size_t> actionIndex;
        actionIndex.reserve(doc.actions.size());
        for (std::size_t i = 0; i < doc.actions.size(); ++i) {
            const auto& a = doc.actions[i];
            if (a.id.empty()) {
                errorOut = "action #" + std::to_string(i) + " has an empty id";
                return false;
            }
            if (!actionIndex.emplace(a.id, i).second) {
                errorOut = "duplicate action id '" + a.id + "'";
                return false;
            }
        }
        for (const auto& menu : doc.menus) {
            if (menu.id.empty()) {
                errorOut = "menu with an empty id";
                return false;
            }
            for (const auto& actionId : menu.actionIds) {
                if (actionIndex.find(actionId) == actionIndex.end()) {
                    errorOut = "menu '" + menu.id + "' references unknown action '" +
                               actionId + "'";
                    return false;
                }
            }
        }
        for (const auto& tb : doc.toolbars) {
            if (tb.id.empty()) {
                errorOut = "toolbar with an empty id";
                return false;
            }
            for (const auto& actionId : tb.actionIds) {
                if (actionIndex.find(actionId) == actionIndex.end()) {
                    errorOut = "toolbar '" + tb.id + "' references unknown action '" +
                               actionId + "'";
                    return false;
                }
            }
        }

        // All-or-nothing: commit only after every check passed.
        doc_ = doc;
        dockIndex_ = std::move(dockIndex);
        actionIndex_ = std::move(actionIndex);
        errorOut.clear();
        return true;
    }

    const QtDockSpec* dock(const std::string& objectName) const override {
        const auto it = dockIndex_.find(objectName);
        if (it == dockIndex_.end()) return nullptr;
        return &doc_.docks[it->second];
    }

    const QtActionSpec* action(const std::string& id) const override {
        const auto it = actionIndex_.find(id);
        if (it == actionIndex_.end()) return nullptr;
        return &doc_.actions[it->second];
    }

    bool set_dock_area(const std::string& objectName, QtDockArea area) override {
        const auto it = dockIndex_.find(objectName);
        if (it == dockIndex_.end()) return false;
        doc_.docks[it->second].area = area;
        return true;
    }

    bool set_dock_visible(const std::string& objectName, bool visible) override {
        const auto it = dockIndex_.find(objectName);
        if (it == dockIndex_.end()) return false;
        doc_.docks[it->second].visible = visible;
        return true;
    }

    bool set_action_enabled(const std::string& id, bool enabled) override {
        const auto it = actionIndex_.find(id);
        if (it == actionIndex_.end()) return false;
        doc_.actions[it->second].enabled = enabled;
        return true;
    }

    bool set_action_checked(const std::string& id, bool checked) override {
        const auto it = actionIndex_.find(id);
        if (it == actionIndex_.end()) return false;
        if (!doc_.actions[it->second].checkable) return false;
        checkedState_[id] = checked;
        return true;
    }

    void set_status(const QtStatusSpec& status) override {
        doc_.status = status;
    }

    QtEditorDocSnapshot snapshot() const override { return doc_; }

    std::string to_json() const override {
        std::ostringstream os;
        os << "{\"version\":\"" << json_escape(doc_.version) << "\",\"docks\":[";
        for (std::size_t i = 0; i < doc_.docks.size(); ++i) {
            if (i) os << ",";
            const auto& d = doc_.docks[i];
            os << "{\"objectName\":\"" << json_escape(d.objectName)
               << "\",\"title\":\"" << json_escape(d.title)
               << "\",\"category\":\"" << json_escape(d.category)
               << "\",\"area\":\"" << qt_dock_area_name(d.area)
               << "\",\"visible\":" << (d.visible ? "true" : "false")
               << ",\"tabified\":" << (d.tabified ? "true" : "false")
               << ",\"closable\":" << (d.closable ? "true" : "false")
               << ",\"movable\":" << (d.movable ? "true" : "false")
               << ",\"floatable\":" << (d.floatable ? "true" : "false") << "}";
        }
        os << "],\"actions\":[";
        for (std::size_t i = 0; i < doc_.actions.size(); ++i) {
            if (i) os << ",";
            const auto& a = doc_.actions[i];
            const bool checked = checkedState_.count(a.id) && checkedState_.at(a.id);
            os << "{\"id\":\"" << json_escape(a.id) << "\",\"text\":\""
               << json_escape(a.text) << "\",\"shortcut\":\""
               << json_escape(a.shortcut) << "\",\"category\":\""
               << json_escape(a.category) << "\",\"action\":\""
               << json_escape(a.action) << "\",\"checkable\":"
               << (a.checkable ? "true" : "false")
               << ",\"enabled\":" << (a.enabled ? "true" : "false")
               << ",\"checked\":" << (checked ? "true" : "false") << "}";
        }
        os << "],\"menus\":[";
        for (std::size_t i = 0; i < doc_.menus.size(); ++i) {
            if (i) os << ",";
            const auto& m = doc_.menus[i];
            os << "{\"id\":\"" << json_escape(m.id) << "\",\"title\":\""
               << json_escape(m.title) << "\",\"actions\":[";
            for (std::size_t j = 0; j < m.actionIds.size(); ++j) {
                if (j) os << ",";
                os << "\"" << json_escape(m.actionIds[j]) << "\"";
            }
            os << "]}";
        }
        os << "],\"toolbars\":[";
        for (std::size_t i = 0; i < doc_.toolbars.size(); ++i) {
            if (i) os << ",";
            const auto& tb = doc_.toolbars[i];
            os << "{\"id\":\"" << json_escape(tb.id) << "\",\"actions\":[";
            for (std::size_t j = 0; j < tb.actionIds.size(); ++j) {
                if (j) os << ",";
                os << "\"" << json_escape(tb.actionIds[j]) << "\"";
            }
            os << "]}";
        }
        os << "],\"status\":{\"state\":\"" << json_escape(doc_.status.state)
           << "\",\"scene\":\"" << json_escape(doc_.status.sceneName)
           << "\",\"entities\":" << doc_.status.entityCount
           << ",\"frameMillis\":" << doc_.status.frameMillis << "}}";
        return os.str();
    }

private:
    QtEditorDocSnapshot doc_;
    std::unordered_map<std::string, std::size_t> dockIndex_;
    std::unordered_map<std::string, std::size_t> actionIndex_;
    std::unordered_map<std::string, bool> checkedState_;  // checkable actions
};

}  // namespace

std::unique_ptr<IQtEditorDoc> create_qt_editor_doc() {
    return std::make_unique<QtEditorDocImpl>();
}

}  // namespace ui
}  // namespace engine
