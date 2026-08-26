#pragma once

// IQtEditorDoc (agente 2 §B — porte Qt, decisão do usuário): the PUBLIC,
// deterministic model of the Qt editor shell document. This is the headless
// core of the user-requested Qt frontend port ("port 100% of the current UI
// plus the wiGUI functionality, but in Qt"): a QMainWindow-based shell
// consumes this document to build its QDockWidgets, QActions, QMenus and
// QToolBars. The contract owns only the DATA, expressed in Qt-native
// semantics — the shell turns it into widgets.
//   - docks: QDockWidget analogs (objectName/title/category, area
//     Left/Right/Top/Bottom/Floating, visible/tabified/closable/movable/
//     floatable) — one per editor panel (the EditorPlugin registry + the
//     wiGUI panel queue).
//   - actions: QAction analogs (id/text/shortcut/category/action,
//     checkable/enabled) — one per palette command.
//   - menus/toolbars: deterministic ordered structures referencing action
//     ids (QMenu/QToolBar analogs).
//   - status: the live shell state a status bar shows (play mode, scene
//     name, entity count, frame time in millis).
// UNEQUIVOCAL: mutations are all-or-nothing — duplicate ids and references
// to unknown ids are refused with a reason, leaving the document untouched.
// DETERMINISM: pure document model, no clocks/RNG; same inputs -> identical
// to_json() output, bit-exact (no floats). OBSERVABLE: the editor exposes
// the document via GET /qt-doc.
//
// Self-contained (std only). The SDK adapter (src/engine/sdk/QtEditorDoc.cpp)
// is the ONLY TU with behavior.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace ui {

// QDockWidget::DockWidgetArea analog.
enum class QtDockArea : std::uint8_t { Left, Right, Top, Bottom, Floating };

const char* qt_dock_area_name(QtDockArea area);

// One QDockWidget the shell should create.
struct QtDockSpec {
    std::string objectName;  // unique, stable (the panel id, e.g. "hierarchy")
    std::string title;       // dock title (the panel title)
    std::string category;    // menu grouping ("" = uncategorized)
    QtDockArea area{ QtDockArea::Left };
    bool visible{ false };   // initial visibility
    bool tabified{ false };  // shares a tab bar with the previous dock
    bool closable{ true };
    bool movable{ true };
    bool floatable{ true };

    bool operator==(const QtDockSpec& other) const {
        return objectName == other.objectName && title == other.title &&
               category == other.category && area == other.area &&
               visible == other.visible && tabified == other.tabified &&
               closable == other.closable && movable == other.movable &&
               floatable == other.floatable;
    }
    bool operator!=(const QtDockSpec& other) const { return !(*this == other); }
};

// One QAction the shell should create.
struct QtActionSpec {
    std::string id;          // unique, stable (the command id, e.g. "scene.save")
    std::string text;        // display text
    std::string shortcut;    // humanized shortcut, "" = none
    std::string category;    // menu group ("" = uncategorized)
    std::string action;      // command the shell runs on trigger ("" = none)
    bool checkable{ false };
    bool enabled{ true };

    bool operator==(const QtActionSpec& other) const {
        return id == other.id && text == other.text &&
               shortcut == other.shortcut && category == other.category &&
               action == other.action && checkable == other.checkable &&
               enabled == other.enabled;
    }
    bool operator!=(const QtActionSpec& other) const { return !(*this == other); }
};

// One QMenu (ordered action references).
struct QtMenuSpec {
    std::string id;                 // e.g. "file", "edit", "view", "tools"
    std::string title;              // menu title
    std::vector<std::string> actionIds;  // ordered (may be empty)

    bool operator==(const QtMenuSpec& other) const {
        return id == other.id && title == other.title &&
               actionIds == other.actionIds;
    }
    bool operator!=(const QtMenuSpec& other) const { return !(*this == other); }
};

// One QToolBar (ordered action references).
struct QtToolbarSpec {
    std::string id;                 // e.g. "main"
    std::vector<std::string> actionIds;  // ordered (may be empty)

    bool operator==(const QtToolbarSpec& other) const {
        return id == other.id && actionIds == other.actionIds;
    }
    bool operator!=(const QtToolbarSpec& other) const { return !(*this == other); }
};

// The live shell state a status bar shows. frameMillis replaces a float fps
// so the document stays bit-exact.
struct QtStatusSpec {
    std::string state;              // "edit"/"play"/"pause"/"simulate"/""
    std::string sceneName;          // "" = no scene
    std::uint64_t entityCount{ 0 };
    std::uint64_t frameMillis{ 0 };  // last frame time, milliseconds

    bool operator==(const QtStatusSpec& other) const {
        return state == other.state && sceneName == other.sceneName &&
               entityCount == other.entityCount &&
               frameMillis == other.frameMillis;
    }
    bool operator!=(const QtStatusSpec& other) const { return !(*this == other); }
};

// The full shell document snapshot.
struct QtEditorDocSnapshot {
    std::string version;             // e.g. "qt-editor-doc-1"
    std::vector<QtDockSpec> docks;
    std::vector<QtActionSpec> actions;
    std::vector<QtMenuSpec> menus;
    std::vector<QtToolbarSpec> toolbars;
    QtStatusSpec status;

    bool operator==(const QtEditorDocSnapshot& other) const {
        return version == other.version && docks == other.docks &&
               actions == other.actions && menus == other.menus &&
               toolbars == other.toolbars && status == other.status;
    }
    bool operator!=(const QtEditorDocSnapshot& other) const {
        return !(*this == other);
    }
};

class IQtEditorDoc {
public:
    virtual ~IQtEditorDoc() = default;

    // Installs the whole document (all-or-nothing). REFUSED (returns false,
    // document untouched, reason in errorOut) when: the version is empty,
    // docks or actions are empty, a dock objectName is duplicated, an action
    // id is duplicated, a menu/toolbar id is duplicated, or a menu/toolbar
    // references an unknown action id.
    virtual bool set_doc(const QtEditorDocSnapshot& doc,
                         std::string& errorOut) = 0;

    // Const accessors (nullptr when unknown).
    virtual const QtDockSpec* dock(const std::string& objectName) const = 0;
    virtual const QtActionSpec* action(const std::string& id) const = 0;

    // All-or-nothing mutations. Each returns false and leaves the document
    // untouched when the target is unknown (or, for set_action_checked, when
    // the action is not checkable).
    virtual bool set_dock_area(const std::string& objectName,
                               QtDockArea area) = 0;
    virtual bool set_dock_visible(const std::string& objectName,
                                  bool visible) = 0;
    virtual bool set_action_enabled(const std::string& id, bool enabled) = 0;
    virtual bool set_action_checked(const std::string& id, bool checked) = 0;

    // Replaces the live status (always succeeds; the shell refreshes it every
    // frame).
    virtual void set_status(const QtStatusSpec& status) = 0;

    virtual QtEditorDocSnapshot snapshot() const = 0;

    // Deterministic JSON of the whole document (bit-exact, no floats).
    virtual std::string to_json() const = 0;
};

// Factory: the SDK adapter is the only TU with behavior.
std::unique_ptr<IQtEditorDoc> create_qt_editor_doc();

}  // namespace ui
}  // namespace engine
