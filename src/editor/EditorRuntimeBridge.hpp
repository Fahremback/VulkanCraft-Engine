#pragma once

// ---------------------------------------------------------------------------
// EditorRuntimeBridge.hpp
//
// Editor → runtime bridge (Play In Editor, README section 36). The editor
// owns the Editor World; the bridge launches the runtime against a separate
// Play World cloned from it (Scene::clone_for_play), runs simulation ticks,
// and destroys the Play World on exit — the Editor World is never mutated
// and stays intact (verified by editor_world_intact()).
//
// Integrates Engine::PlayModeManager (engine/editor/play_mode/PlayMode.hpp)
// for the core state machine and adds what the manager lacks:
//   • a stored tick callback (simulation step)
//   • simulate_ticks(N, dt) — run N frames in one burst then restore
//   • a tick counter and an Editor-World integrity check
//
// Modes: Edit → Playing / Simulating → Paused (step frame) → back to Edit.
// ---------------------------------------------------------------------------

#include "engine/editor/play_mode/PlayMode.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace Engine::Editor {

class EditorRuntimeBridge final {
public:
    enum class Mode { Edit, Playing, Paused, Simulating };

    using TickCallback = std::function<void(Scene*, float)>;

    EditorRuntimeBridge();
    ~EditorRuntimeBridge();
    EditorRuntimeBridge(const EditorRuntimeBridge&) = delete;
    EditorRuntimeBridge& operator=(const EditorRuntimeBridge&) = delete;

    /// Simulation step invoked on the Play World every tick. Receives the
    /// play scene and the fixed/elapsed delta time.
    void set_tick_callback(TickCallback tick) { tick_ = std::move(tick); }

    // --- Play-mode lifecycle --------------------------------------------
    /// Clone `editorScene` into a Play World and switch to Playing mode.
    bool enter_play(Scene& editorScene);
    /// Same but for Simulate mode (systems run, no player input).
    bool enter_simulate(Scene& editorScene);
    bool pause();
    bool resume();
    /// Run exactly one simulation tick on the Play World (Paused mode only).
    bool step_frame(float deltaTime);
    /// Run `tickCount` simulation ticks. From Edit mode this enters Play,
    /// ticks, then restores Edit (Play World destroyed, Editor World intact).
    /// While already playing it just ticks the current Play World N times.
    size_t simulate_ticks(Scene& editorScene, size_t tickCount, float deltaTime);
    /// Destroy the Play World and return to Edit mode.
    bool exit_play();
    void destroy() { exit_play(); }

    // --- Introspection --------------------------------------------------
    Mode mode() const { return mode_; }
    PlayState play_state() const { return manager_->get_state(); }
    Scene* play_world();
    const Scene* play_world() const;
    Scene* editor_world() const { return editorScene_; }
    size_t ticks_elapsed() const { return ticks_; }
    /// True while the Editor World still matches the snapshot taken at
    /// enter_play (i.e. simulation never leaked into the edited scene).
    bool editor_world_intact() const;

private:
    void tick_once(float deltaTime);
    bool capture_editor_signature();
    static std::string scene_signature(const Scene& scene);

    std::unique_ptr<PlayModeManager> manager_;
    TickCallback tick_;
    Scene* editorScene_{ nullptr };
    Mode mode_{ Mode::Edit };
    Mode pausedFrom_{ Mode::Edit };
    size_t ticks_{ 0 };
    std::string editorSignature_;
};

} // namespace Engine::Editor
