// EditorApplicationCameraGizmo.cpp
//
// Agente 3 (fechamento_solidacao) — TU split: cohesive camera/gizmo/viewport methods were
// extracted verbatim from the 209KB EditorApplicationRecovered.cpp (behavior
// preserved; CMake still compiles this TU into VulkanEngineEditor).
#include "EditorApplication.hpp"
#include "EditorInternalHelpers.hpp"
#include "EditorApplicationRecoveredShared.hpp"
#include "../engine/assets/GltfGeometry.hpp"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <windows.h>
#include <shlobj.h>
#include <sstream>
#include <vector>
#include <fstream>
#include <filesystem>
#include <cstdlib>

namespace Engine {

void EditorApplication::update_editor_camera(float deltaTime) {
    // Respond to the mouse over the rendered image, not to ImGui window focus:
    // focus can go stale (another panel taking it), which made the viewport
    // appear to stop answering the mouse entirely.
    if (!m_viewportImageHovered) return;

    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(m_window, &mx, &my);
    const glm::vec2 mouse(static_cast<float>(mx), static_cast<float>(my));
    const glm::vec2 mouseDelta = mouse - m_lastMousePos;
    m_lastMousePos = mouse;

    EditorCamera& cam = m_editorCamera;
    const glm::vec3 front = cam.get_front();
    const glm::vec3 right = cam.get_right();
    const glm::vec3 up = cam.get_up();

    // Don't let camera keys fight the user typing in Inspector/text fields.
    // NOTE: io.WantCaptureKeyboard is true whenever the mouse hovers ANY
    // window, which would kill WASD the moment the cursor is over the 3D view;
    // io.WantTextInput is true only while an actual text field is being typed.
    ImGuiIO& io = ImGui::GetIO();
    const bool keysFree = !io.WantTextInput;

    // Orbit (right drag) / pan (middle drag).
    const bool orbitHeld = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    const bool panHeld = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    if (orbitHeld && !m_gizmoDragging) {
        cam.yaw += mouseDelta.x * cam.sensitivity;
        cam.pitch = glm::clamp(cam.pitch - mouseDelta.y * cam.sensitivity, -89.0f, 89.0f);
    }
    if (panHeld) {
        const float panScale = cam.orbitDistance * 0.0016f;
        cam.orbitTarget += (-right * mouseDelta.x + up * mouseDelta.y) * panScale;
    }

    // Fly (WASD): free-fly whenever the mouse is over the viewport and the
    // keyboard is not captured by a text field — no right-button required.
    if (keysFree) {
        const float speed = cam.speed * (glfwGetKey(m_window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 4.0f : 1.0f);
        glm::vec3 move(0.0f);
        if (glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS) move += front;
        if (glfwGetKey(m_window, GLFW_KEY_S) == GLFW_PRESS) move -= front;
        if (glfwGetKey(m_window, GLFW_KEY_D) == GLFW_PRESS) move += right;
        if (glfwGetKey(m_window, GLFW_KEY_A) == GLFW_PRESS) move -= right;
        if (glfwGetKey(m_window, GLFW_KEY_E) == GLFW_PRESS) move += up;
        if (glfwGetKey(m_window, GLFW_KEY_Q) == GLFW_PRESS) move -= up;
        if (glm::length(move) > 0.0f) {
            cam.orbitTarget += glm::normalize(move) * speed * deltaTime;
        }
    }

    // Scroll zoom: the wheel inside the 3D view ALWAYS dollies toward/away
    // from the orbit focus — it never scrolls any panel (the viewport is
    // NoScrollbar|NoScrollWithMouse, and the delta is consumed here). The
    // delta comes from our own GLFW callback accumulator, not io.MouseWheel,
    // which ImGui zeroes at the end of NewFrame before we can read it. When
    // the viewport is NOT hovered the accumulator is dropped so ImGui keeps
    // scrolling other panels normally.
    if (m_viewportHovered || m_viewportImageHovered) {
        if (m_scrollAccum != 0.0) {
            cam.orbitDistance = glm::clamp(
                cam.orbitDistance * (1.0f - static_cast<float>(m_scrollAccum) * 0.1f), 0.5f, 5000.0f);
            m_scrollAccum = 0.0;
        }
    } else {
        m_scrollAccum = 0.0;
    }

    recompute_editor_camera_position();
}


void EditorApplication::process_viewport_input() {
    // Gizmo keys work on hover (mouse over the 3D image), not on ImGui window
    // focus — focus can sit on another panel and would freeze the keys.
    if (!m_viewportImageHovered) return;
    ImGuiIO& io = ImGui::GetIO();
    // Gizmo mode switching: Q / W / E / R
    if (!io.WantCaptureKeyboard) {
        if (glfwGetKey(m_window, GLFW_KEY_Q) == GLFW_PRESS && m_gizmoMode != GizmoMode::Select) {
            m_gizmoMode = GizmoMode::Select;
        }
        if (glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS && m_gizmoMode != GizmoMode::Translate) {
            m_gizmoMode = GizmoMode::Translate;
        }
        if (glfwGetKey(m_window, GLFW_KEY_E) == GLFW_PRESS && m_gizmoMode != GizmoMode::Rotate) {
            m_gizmoMode = GizmoMode::Rotate;
        }
        if (glfwGetKey(m_window, GLFW_KEY_R) == GLFW_PRESS && m_gizmoMode != GizmoMode::Scale) {
            m_gizmoMode = GizmoMode::Scale;
        }
    }
}


bool EditorApplication::gizmo_axis_hit_test(glm::vec2 mouseScreen) {
    m_hoveredAxis = GizmoAxis::None;
    if (!m_editorScene || !m_selectedEntity.is_valid()) return false;
    const auto it = m_editorScene->transformComponents.find(m_selectedEntity.get_id());
    if (it == m_editorScene->transformComponents.end()) return false;
    const glm::vec3 origin = it->second.position;
    // World/Local hit test: axes rotate with the entity in local mode.
    const glm::quat gizmoRotation = m_gizmoLocal
        ? glm::quat(glm::radians(it->second.rotation))
        : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    const auto axisWorld = [&](int axis) -> glm::vec3 { return gizmoRotation * kAxisDirs[axis]; };

    const float aspect = m_viewportImageSize.x / std::max(1.0f, m_viewportImageSize.y);
    const glm::mat4 viewProj = m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix();
    const auto project = [&](const glm::vec3& world) -> glm::vec2 {
        glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
        if (std::abs(clip.w) < 1e-6f) return { -1e9f, -1e9f };
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        return glm::vec2(m_viewportImagePos.x + (ndc.x * 0.5f + 0.5f) * m_viewportImageSize.x,
                         m_viewportImagePos.y + (-ndc.y * 0.5f + 0.5f) * m_viewportImageSize.y);
    };

    const glm::vec2 originScreen = project(origin);
    float bestDist = 1e18f;
    GizmoAxis best = GizmoAxis::None;
    const float gizmoLen = (m_gizmoMode == GizmoMode::Rotate) ? 1.45f : 1.55f;
    for (int axis = 0; axis < 3; ++axis) {
        float dist = 1e18f;
        if (m_gizmoMode == GizmoMode::Rotate) {
            // Distance to the projected ring polyline.
            for (int s = 0; s < 48; ++s) {
                const float a0 = glm::two_pi<float>() * static_cast<float>(s) / 48.0f;
                const float a1 = glm::two_pi<float>() * static_cast<float>(s + 1) / 48.0f;
                const glm::vec3 dir = axisWorld(axis);
                glm::vec3 u = glm::normalize(glm::cross(dir, std::abs(dir.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
                glm::vec3 v = glm::normalize(glm::cross(dir, u));
                const glm::vec3 p0 = origin + u * (std::cos(a0) * gizmoLen) + v * (std::sin(a0) * gizmoLen);
                const glm::vec3 p1 = origin + u * (std::cos(a1) * gizmoLen) + v * (std::sin(a1) * gizmoLen);
                dist = std::min(dist, dist_point_segment(mouseScreen, project(p0), project(p1)));
            }
        } else {
            const glm::vec2 tipScreen = project(origin + axisWorld(axis) * gizmoLen);
            dist = dist_point_segment(mouseScreen, originScreen, tipScreen);
        }
        if (dist < 14.0f && dist < bestDist) {
            bestDist = dist;
            best = static_cast<GizmoAxis>(axis + 1);
        }
    }
    m_hoveredAxis = best;
    return best != GizmoAxis::None;
}


void EditorApplication::start_gizmo_drag(glm::vec2 mouseScreen) {
    if (!m_editorScene || !m_selectedEntity.is_valid()) return;
    const UUID id = m_selectedEntity.get_id();
    if (!m_editorScene->transformComponents.contains(id)) return;
    const TransformComponent& t = m_editorScene->transformComponents.at(id);

    m_gizmoDragging = true;
    m_gizmoDragEntityStart = t.position;
    m_gizmoDragRotStart = t.rotation;
    m_gizmoDragScaleStart = t.scale;
    // World/Local: in local mode the drag axis follows the entity rotation.
    if (m_gizmoLocal) {
        m_gizmoAxisWorld = glm::quat(glm::radians(t.rotation)) * kAxisDirs[static_cast<int>(m_activeAxis) - 1];
    } else {
        m_gizmoAxisWorld = kAxisDirs[static_cast<int>(m_activeAxis) - 1];
    }
    m_gizmoDragPlaneNormal = glm::normalize(m_editorCamera.orbitTarget - m_editorCamera.position);
    if (glm::length(m_gizmoDragPlaneNormal) < 1e-5f) m_gizmoDragPlaneNormal = glm::vec3(0, 0, 1);

    const float aspect = m_viewportImageSize.x / std::max(1.0f, m_viewportImageSize.y);
    const glm::mat4 invViewProj = glm::inverse(
        m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix());
    m_gizmoDragPlanePoint = unproject_to_plane(mouseScreen, t.position, m_gizmoDragPlaneNormal, invViewProj);

    if (m_gizmoMode == GizmoMode::Rotate) {
        glm::vec3 toPoint = m_gizmoDragPlanePoint - t.position;
        if (glm::length(toPoint) < 1e-5f) toPoint = m_gizmoAxisWorld == glm::vec3(0, 1, 0) ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        glm::vec3 ref = toPoint - m_gizmoAxisWorld * glm::dot(toPoint, m_gizmoAxisWorld);
        if (glm::dot(ref, ref) < 1e-6f) ref = glm::normalize(glm::cross(m_gizmoAxisWorld, glm::vec3(0, 0, 1)));
        m_gizmoDragAngleRef = glm::normalize(ref);
    }
}


void EditorApplication::update_gizmo_drag(glm::vec2 mouseScreen) {
    if (!m_editorScene || !m_selectedEntity.is_valid() || !m_gizmoDragging) return;
    const UUID id = m_selectedEntity.get_id();
    auto it = m_editorScene->transformComponents.find(id);
    if (it == m_editorScene->transformComponents.end()) return;

    const float aspect = m_viewportImageSize.x / std::max(1.0f, m_viewportImageSize.y);
    const glm::mat4 invViewProj = glm::inverse(
        m_editorCamera.get_projection_matrix(aspect) * m_editorCamera.get_view_matrix());
    const glm::vec3 planePoint = unproject_to_plane(mouseScreen, m_gizmoDragPlanePoint,
                                                    m_gizmoDragPlaneNormal, invViewProj);
    const bool snap = ImGui::GetIO().KeyCtrl;
    const int axisIndex = static_cast<int>(m_activeAxis) - 1;

    if (m_gizmoMode == GizmoMode::Translate) {
        float delta = glm::dot(planePoint - m_gizmoDragPlanePoint, m_gizmoAxisWorld);
        if (snap) delta = std::round(delta / m_snapTranslate) * m_snapTranslate;
        const glm::vec3 newPos = m_gizmoDragEntityStart + m_gizmoAxisWorld * delta;
        m_undo.execute_or_merge_property(
            "Move Entity",
            [this, id, newPos] { auto it = m_editorScene->transformComponents.find(id); if (it != m_editorScene->transformComponents.end()) it->second.position = newPos; },
            [this, id, start = m_gizmoDragEntityStart] {
                auto it = m_editorScene->transformComponents.find(id); if (it != m_editorScene->transformComponents.end()) it->second.position = start;
            });
    } else if (m_gizmoMode == GizmoMode::Rotate) {
        glm::vec3 toPoint = planePoint - m_gizmoDragEntityStart;
        if (glm::length(toPoint) < 1e-5f) return;
        glm::vec3 v = toPoint - m_gizmoAxisWorld * glm::dot(toPoint, m_gizmoAxisWorld);
        if (glm::dot(v, v) < 1e-6f) return;
        v = glm::normalize(v);
        const float angle = glm::degrees(std::atan2(
            glm::dot(glm::cross(m_gizmoDragAngleRef, v), m_gizmoAxisWorld),
            glm::dot(m_gizmoDragAngleRef, v)));
        const float snapped = snap ? std::round(angle / m_snapRotate) * m_snapRotate : angle;
        glm::vec3 newRot = m_gizmoDragRotStart;
        newRot[axisIndex] += snapped;
        m_undo.execute_or_merge_property(
            "Rotate Entity",
            [this, id, newRot] { auto it = m_editorScene->transformComponents.find(id); if (it != m_editorScene->transformComponents.end()) it->second.rotation = newRot; },
            [this, id, start = m_gizmoDragRotStart] {
                auto it = m_editorScene->transformComponents.find(id); if (it != m_editorScene->transformComponents.end()) it->second.rotation = start;
            });
    } else if (m_gizmoMode == GizmoMode::Scale) {
        float delta = glm::dot(planePoint - m_gizmoDragPlanePoint, m_gizmoAxisWorld);
        float factor = 1.0f + delta / 1.0f;
        if (snap) factor = std::round(factor / m_snapScale) * m_snapScale;
        factor = std::max(factor, 0.02f);
        glm::vec3 newScale = m_gizmoDragScaleStart;
        newScale[axisIndex] = m_gizmoDragScaleStart[axisIndex] * factor;
        m_undo.execute_or_merge_property(
            "Scale Entity",
            [this, id, newScale] { auto it = m_editorScene->transformComponents.find(id); if (it != m_editorScene->transformComponents.end()) it->second.scale = newScale; },
            [this, id, start = m_gizmoDragScaleStart] {
                auto it = m_editorScene->transformComponents.find(id); if (it != m_editorScene->transformComponents.end()) it->second.scale = start;
            });
    }
}


void EditorApplication::recompute_editor_camera_position() {
    // Recompute the camera position from target + spherical offset.
    m_editorCamera.position = m_editorCamera.orbitTarget -
                              euler_direction(m_editorCamera.yaw, m_editorCamera.pitch) *
                              m_editorCamera.orbitDistance;
}


glm::vec3 EditorApplication::unproject_to_plane(glm::vec2 mouseScreen, const glm::vec3& planePoint,
                                                const glm::vec3& planeNormal, const glm::mat4& invViewProj) const {
    const float ndcX = (mouseScreen.x - m_viewportImagePos.x) / std::max(1.0f, m_viewportImageSize.x) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (mouseScreen.y - m_viewportImagePos.y) / std::max(1.0f, m_viewportImageSize.y) * 2.0f;
    const glm::vec4 near4 = invViewProj * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    const glm::vec4 far4 = invViewProj * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    const glm::vec3 nearP = glm::vec3(near4) / near4.w;
    const glm::vec3 farP = glm::vec3(far4) / far4.w;
    const glm::vec3 dir = glm::normalize(farP - nearP);
    const float denom = glm::dot(dir, planeNormal);
    if (std::abs(denom) < 1e-6f) return planePoint;
    const float t = glm::dot(planePoint - nearP, planeNormal) / denom;
    return nearP + dir * t;
}


}  // namespace Engine
