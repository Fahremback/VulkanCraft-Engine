#pragma once

#include <memory>
#include "../../scene/Scene.hpp"

namespace Engine {

enum class PlayState {
    Edit,
    Play,
    Pause,
    Simulate
};

class PlayModeManager {
public:
    PlayModeManager() = default;
    PlayModeManager(const PlayModeManager&) = delete;
    PlayModeManager& operator=(const PlayModeManager&) = delete;

    PlayState get_state() const { return m_state; }

    // Editor-only: used by the PASSO (frame-step) button to advance the play
    // world one frame while paused (the editor flips Pause→Play, ticks once,
    // and flips back).
    void set_state(PlayState state) { m_state = state; }

    // Editor-only: keeps the cached editor-scene pointer fresh so
    // get_active_scene() returns the real scene in Edit mode even before the
    // first start_play() (which would otherwise leave it null and make every
    // caller see an empty scene). The editor calls this whenever it replaces
    // its scene (init, New Scene, Open Scene, after stop_play).
    void set_editor_scene(Scene* editorScene) { m_editorScene = editorScene; }

    void start_play(Scene* editorScene) {
        if (m_state != PlayState::Edit) return;
        m_editorScene = editorScene;
        m_playScene = std::make_unique<Scene>(editorScene->clone_for_play());
        m_state = PlayState::Play;
    }

    void start_simulate(Scene* editorScene) {
        if (m_state != PlayState::Edit) return;
        m_editorScene = editorScene;
        m_playScene = std::make_unique<Scene>(editorScene->clone_for_play());
        m_state = PlayState::Simulate;
    }

    void pause_play() {
        if (m_state == PlayState::Play || m_state == PlayState::Simulate) {
            m_state = PlayState::Pause;
        } else if (m_state == PlayState::Pause) {
            m_state = PlayState::Play;
        }
    }

    void step_frame(float deltaTime, const std::function<void(Scene*, float)>& tickCallback) {
        if (m_state != PlayState::Pause) return;
        if (m_playScene && tickCallback) {
            tickCallback(m_playScene.get(), deltaTime);
        }
    }

    void stop_play() {
        if (m_state == PlayState::Edit) return;
        m_playScene.reset();
        // The cached editor-scene pointer was captured at start_play and can
        // dangle the moment the editor replaces its scene (e.g. New Scene) —
        // clear it so get_active_scene() never hands out a freed Scene. In
        // Edit mode callers fall back to their own current scene.
        m_editorScene = nullptr;
        m_state = PlayState::Edit;
    }

    Scene* get_active_scene() {
        if (m_state == PlayState::Play || m_state == PlayState::Pause || m_state == PlayState::Simulate) {
            return m_playScene.get();
        }
        return m_editorScene;
    }

private:
    PlayState m_state{ PlayState::Edit };
    Scene* m_editorScene{ nullptr };
    std::unique_ptr<Scene> m_playScene;
};

} // namespace Engine
