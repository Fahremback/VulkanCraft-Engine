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
