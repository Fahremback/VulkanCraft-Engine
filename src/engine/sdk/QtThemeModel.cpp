// IQtThemeModel adapter (agente 2 §B — porte Qt): the Wicked charcoal theme
// in Qt-native form (QPalette roles + QSS). derive_charcoal_palette() applies
// the same lift math the ImGui theme uses (panel + 8% for frames/buttons), so
// the Qt shell and the ImGui editor stay in sync by construction. All colors
// are bytes; JSON is bit-exact.

#include "engine/ui/qt/IQtThemeModel.hpp"

#include <cstdio>
#include <sstream>
#include <unordered_map>

namespace engine {
namespace ui {

const char* qt_palette_role_name(QtPaletteRole role) {
    switch (role) {
        case QtPaletteRole::Window: return "Window";
        case QtPaletteRole::WindowText: return "WindowText";
        case QtPaletteRole::Base: return "Base";
        case QtPaletteRole::AlternateBase: return "AlternateBase";
        case QtPaletteRole::Text: return "Text";
        case QtPaletteRole::Button: return "Button";
        case QtPaletteRole::ButtonText: return "ButtonText";
        case QtPaletteRole::Highlight: return "Highlight";
        case QtPaletteRole::HighlightedText: return "HighlightedText";
        case QtPaletteRole::PlaceholderText: return "PlaceholderText";
        case QtPaletteRole::ToolTipBase: return "ToolTipBase";
        case QtPaletteRole::ToolTipText: return "ToolTipText";
    }
    return "Window";
}

std::string rgba_to_hex(const QtRgba& color) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", color.r, color.g, color.b);
    return buf;
}

namespace {

// Lifts each channel by `frac` of 255 (saturating). Same math shape the ImGui
// theme uses (panel + 0.08 for frames/buttons).
QtRgba lift(const QtRgba& base, float frac) {
    auto ch = [frac](std::uint8_t v) -> std::uint8_t {
        const int out = static_cast<int>(v) + static_cast<int>(frac * 255.0f + 0.5f);
        return static_cast<std::uint8_t>(out < 0 ? 0 : (out > 255 ? 255 : out));
    };
    return QtRgba{ ch(base.r), ch(base.g), ch(base.b), base.a };
}

QtRgba dim(const QtRgba& base, float frac) {
    auto ch = [frac](std::uint8_t v) -> std::uint8_t {
        const int out = static_cast<int>(v * frac + 0.5f);
        return static_cast<std::uint8_t>(out < 0 ? 0 : (out > 255 ? 255 : out));
    };
    return QtRgba{ ch(base.r), ch(base.g), ch(base.b), base.a };
}

QtRgba lerp(const QtRgba& a, const QtRgba& b, float t) {
    auto ch = [&](std::uint8_t x, std::uint8_t y) -> std::uint8_t {
        const int out = static_cast<int>(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t + 0.5f);
        return static_cast<std::uint8_t>(out < 0 ? 0 : (out > 255 ? 255 : out));
    };
    return QtRgba{ ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), a.a };
}

const QtRgba kAccent{ 96, 160, 255, 255 };  // documented neutral blue

class QtThemeModelImpl final : public IQtThemeModel {
public:
    QtThemeModelImpl() = default;

    bool set_theme(const QtThemeSnapshot& theme, std::string& errorOut) override {
        if (theme.name.empty()) {
            errorOut = "theme name must not be empty";
            return false;
        }
        bool hasWindow = false, hasBase = false, hasText = false,
             hasButton = false, hasHighlight = false;
        std::unordered_map<std::uint8_t, std::size_t> seen;
        for (std::size_t i = 0; i < theme.palette.size(); ++i) {
            const auto& [role, color] = theme.palette[i];
            const auto key = static_cast<std::uint8_t>(role);
            if (seen.find(key) != seen.end()) {
                errorOut = "duplicate palette role '" +
                           std::string(qt_palette_role_name(role)) + "'";
                return false;
            }
            seen.emplace(key, i);
            switch (role) {
                case QtPaletteRole::Window: hasWindow = true; break;
                case QtPaletteRole::Base: hasBase = true; break;
                case QtPaletteRole::Text: hasText = true; break;
                case QtPaletteRole::Button: hasButton = true; break;
                case QtPaletteRole::Highlight: hasHighlight = true; break;
                default: break;
            }
            (void)color;
        }
        if (!hasWindow || !hasBase || !hasText || !hasButton || !hasHighlight) {
            errorOut = "palette must include Window/Base/Text/Button/Highlight";
            return false;
        }
        std::unordered_map<std::string, std::size_t> selectors;
        for (std::size_t i = 0; i < theme.qss.size(); ++i) {
            if (theme.qss[i].selector.empty()) {
                errorOut = "qss rule with an empty selector";
                return false;
            }
            if (!selectors.emplace(theme.qss[i].selector, i).second) {
                errorOut = "duplicate qss selector '" + theme.qss[i].selector + "'";
                return false;
            }
        }
        name_ = theme.name;
        palette_ = theme.palette;
        qss_ = theme.qss;
        roleIndex_.clear();
        for (std::size_t i = 0; i < palette_.size(); ++i) {
            roleIndex_.emplace(static_cast<std::uint8_t>(palette_[i].first), i);
        }
        errorOut.clear();
        return true;
    }

    bool set_qss(const std::vector<QtQssRule>& qss, std::string& errorOut) override {
        std::unordered_map<std::string, std::size_t> selectors;
        for (std::size_t i = 0; i < qss.size(); ++i) {
            if (qss[i].selector.empty()) {
                errorOut = "qss rule with an empty selector";
                return false;
            }
            if (!selectors.emplace(qss[i].selector, i).second) {
                errorOut = "duplicate qss selector '" + qss[i].selector + "'";
                return false;
            }
        }
        qss_ = qss;
        errorOut.clear();
        return true;
    }

    const QtRgba* color(QtPaletteRole role) const override {
        const auto it = roleIndex_.find(static_cast<std::uint8_t>(role));
        if (it == roleIndex_.end()) return nullptr;
        return &palette_[it->second].second;
    }

    const QtQssRule* qss_rule(const std::string& selector) const override {
        for (const auto& rule : qss_) {
            if (rule.selector == selector) return &rule;
        }
        return nullptr;
    }

    QtThemeSnapshot snapshot() const override {
        return QtThemeSnapshot{ name_, palette_, qss_ };
    }

    std::string to_json() const override {
        std::ostringstream os;
        os << "{\"name\":\"" << name_ << "\",\"palette\":{";
        for (std::size_t i = 0; i < palette_.size(); ++i) {
            if (i) os << ",";
            os << "\"" << qt_palette_role_name(palette_[i].first) << "\":\""
               << rgba_to_hex(palette_[i].second) << "\"";
        }
        os << "},\"qss\":[";
        for (std::size_t i = 0; i < qss_.size(); ++i) {
            if (i) os << ",";
            os << "{\"selector\":\"" << qss_[i].selector
               << "\",\"declarations\":\"" << qss_[i].declarations << "\"}";
        }
        os << "]}";
        return os.str();
    }

private:
    std::string name_{ "charcoal" };
    std::vector<std::pair<QtPaletteRole, QtRgba>> palette_;
    std::vector<QtQssRule> qss_;
    std::unordered_map<std::uint8_t, std::size_t> roleIndex_;
};

}  // namespace

std::vector<std::pair<QtPaletteRole, QtRgba>> derive_charcoal_palette(
    QtRgba bg, QtRgba panel, QtRgba text) {
    std::vector<std::pair<QtPaletteRole, QtRgba>> out;
    out.reserve(12);
    out.emplace_back(QtPaletteRole::Window, bg);
    out.emplace_back(QtPaletteRole::WindowText, text);
    out.emplace_back(QtPaletteRole::Base, panel);
    out.emplace_back(QtPaletteRole::AlternateBase, lift(panel, 0.04f));
    out.emplace_back(QtPaletteRole::Text, text);
    out.emplace_back(QtPaletteRole::Button, lift(panel, 0.08f));
    out.emplace_back(QtPaletteRole::ButtonText, text);
    out.emplace_back(QtPaletteRole::Highlight, kAccent);
    out.emplace_back(QtPaletteRole::HighlightedText,
                     QtRgba{ 255, 255, 255, 255 });
    out.emplace_back(QtPaletteRole::PlaceholderText, dim(text, 0.55f));
    out.emplace_back(QtPaletteRole::ToolTipBase, lift(bg, 0.15f));
    out.emplace_back(QtPaletteRole::ToolTipText, text);
    return out;
}

std::vector<QtQssRule> qss_from_palette(
    const std::vector<std::pair<QtPaletteRole, QtRgba>>& palette) {
    auto find = [&palette](QtPaletteRole role) -> QtRgba {
        for (const auto& [r, c] : palette) {
            if (r == role) return c;
        }
        return QtRgba{ 0, 0, 0, 255 };
    };
    const QtRgba window = find(QtPaletteRole::Window);
    const QtRgba base = find(QtPaletteRole::Base);
    const QtRgba text = find(QtPaletteRole::Text);
    const QtRgba button = find(QtPaletteRole::Button);
    const QtRgba buttonText = find(QtPaletteRole::ButtonText);
    const QtRgba highlight = find(QtPaletteRole::Highlight);
    const QtRgba highlightedText = find(QtPaletteRole::HighlightedText);
    const QtRgba alternate = find(QtPaletteRole::AlternateBase);
    const QtRgba tooltipBase = find(QtPaletteRole::ToolTipBase);
    const QtRgba tooltipText = find(QtPaletteRole::ToolTipText);

    std::vector<QtQssRule> out;
    out.reserve(9);
    out.push_back(QtQssRule{ "QMainWindow",
                             "background:" + rgba_to_hex(window) + ";" });
    out.push_back(QtQssRule{ "QDockWidget",
                             "color:" + rgba_to_hex(text) + ";" });
    out.push_back(QtQssRule{
        "QTreeView, QListView, QTableView",
        "background:" + rgba_to_hex(base) + ";color:" + rgba_to_hex(text) +
            ";alternate-background-color:" + rgba_to_hex(alternate) +
            ";selection-background-color:" + rgba_to_hex(highlight) +
            ";selection-color:" + rgba_to_hex(highlightedText) + ";" });
    out.push_back(QtQssRule{
        "QPushButton, QComboBox, QSpinBox, QDoubleSpinBox, QSlider",
        "background-color:" + rgba_to_hex(button) + ";color:" +
            rgba_to_hex(buttonText) + ";" });
    out.push_back(QtQssRule{
        "QMenuBar, QMenu, QStatusBar, QToolBar",
        "background-color:" + rgba_to_hex(button) + ";color:" +
            rgba_to_hex(buttonText) + ";" });
    out.push_back(QtQssRule{
        "QLineEdit, QTextEdit, QPlainTextEdit",
        "background:" + rgba_to_hex(base) + ";color:" + rgba_to_hex(text) +
            ";selection-background-color:" + rgba_to_hex(highlight) +
            ";selection-color:" + rgba_to_hex(highlightedText) + ";" });
    out.push_back(QtQssRule{ "QToolTip",
                             "background:" + rgba_to_hex(tooltipBase) +
                                 ";color:" + rgba_to_hex(tooltipText) + ";" });
    return out;
}

std::unique_ptr<IQtThemeModel> create_qt_theme_model() {
    return std::make_unique<QtThemeModelImpl>();
}

}  // namespace ui
}  // namespace engine
