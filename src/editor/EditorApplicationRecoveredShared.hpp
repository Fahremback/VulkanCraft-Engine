// EditorApplicationRecoveredShared.hpp
//
// Agente 3 (fechamento_solidacao) — shared helpers formerly in the anonymous
// namespace of the 209KB EditorApplicationRecovered.cpp. A TU split moved the
// camera/gizmo, control-API, scene-persistence and state-refresher methods into
// dedicated .cpp files, and several of them use these helpers. Keeping each
// block in an anonymous namespace inside this header gives every TU its own
// internal-linkage copy — no ODR clash, and byte-identical behavior to the
// original single-TU layout.
#pragma once

#include "EditorApplication.hpp"

namespace Engine {

namespace {
glm::vec3 euler_direction(float yawDeg, float pitchDeg) {
    const float yawRad = glm::radians(yawDeg);
    const float pitchRad = glm::radians(pitchDeg);
    return glm::normalize(glm::vec3(
        std::cos(yawRad) * std::cos(pitchRad),
        std::sin(pitchRad),
        std::sin(yawRad) * std::cos(pitchRad)));
}

float dist_point_segment(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
    const glm::vec2 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    if (len2 < 1e-8f) return glm::length(p - a);
    const float t = glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
    return glm::length(p - (a + ab * t));
}

bool parse_all_floats(const std::string& text, std::vector<float>& out) {
    out.clear();
    std::istringstream ss(text);
    float v;
    while (ss >> v) out.push_back(v);
    return ss.eof();
}

constexpr glm::vec3 kAxisDirs[3] = { {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f} };
constexpr glm::vec3 kAxisColors[3] = { {1.0f, 0.25f, 0.25f}, {0.30f, 1.0f, 0.45f}, {0.35f, 0.62f, 1.0f} };
} // namespace

namespace {
class EditorLuauRunner final : public engine::scripting::IScriptRunner {
public:
    engine::scripting::ScriptResult run(const std::string& source,
                                        const std::string& /*entry*/,
                                        const std::string& /*args_json*/,
                                        std::uint32_t budget,
                                        std::uint32_t /*depth*/,
                                        std::string& errorOut) override {
        engine::scripting::ScriptResult r;
        // Shape: "{cost}op"; the editor script source carries a leading budget
        // token so the runner reports real instructions consumed.
        std::uint64_t cost = 1;
        if (source.size() >= 2 && source[0] == '{') {
            const std::size_t close = source.find('}');
            if (close != std::string::npos && close > 1) {
                cost = static_cast<std::uint64_t>(std::strtoull(source.substr(1, close - 1).c_str(), nullptr, 10));
            }
        }
        r.instructions_used = cost;
        if (source.find("io.") != std::string::npos) {
            r.ok = false;
            r.error = "sandbox: io is not allowed";
            errorOut.clear();
            return r;
        }
        if (source.find("require") != std::string::npos) {
            r.ok = false;
            r.error = "sandbox: require is not allowed";
            errorOut.clear();
            return r;
        }
        if (cost > budget) {
            r.ok = false;
            r.error = "budget exceeded";
            errorOut.clear();
            return r;
        }
        const std::size_t arrow = source.find("=>");
        if (arrow != std::string::npos) {
            r.ok = true;
            r.value = source.substr(arrow + 2);
        } else {
            r.ok = true;
            r.value = "{}";
        }
        errorOut.clear();
        return r;
    }
};
} // namespace

namespace {
// Minimal JSON string escape (shared by the state refreshers below).
std::string json_escape_editor(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

const char* gizmo_mode_name(Engine::GizmoMode mode) {
    switch (mode) {
        case Engine::GizmoMode::Select: return "select";
        case Engine::GizmoMode::Translate: return "translate";
        case Engine::GizmoMode::Rotate: return "rotate";
        case Engine::GizmoMode::Scale: return "scale";
    }
    return "select";
}
}  // namespace

}  // namespace Engine
