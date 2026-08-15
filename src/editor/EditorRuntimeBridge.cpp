#include "EditorRuntimeBridge.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <vector>

namespace Engine::Editor {

EditorRuntimeBridge::EditorRuntimeBridge()
    : manager_(std::make_unique<PlayModeManager>()) {}

EditorRuntimeBridge::~EditorRuntimeBridge() = default;

bool EditorRuntimeBridge::enter_play(Scene& editorScene) {
    if (mode_ != Mode::Edit) return false;
    manager_->start_play(&editorScene);
    if (manager_->get_state() != PlayState::Play) return false;
    editorScene_ = &editorScene;
    mode_ = Mode::Playing;
    ticks_ = 0;
    capture_editor_signature();
    return true;
}

bool EditorRuntimeBridge::enter_simulate(Scene& editorScene) {
    if (mode_ != Mode::Edit) return false;
    manager_->start_simulate(&editorScene);
    if (manager_->get_state() != PlayState::Simulate) return false;
    editorScene_ = &editorScene;
    mode_ = Mode::Simulating;
    ticks_ = 0;
    capture_editor_signature();
    return true;
}

bool EditorRuntimeBridge::pause() {
    if (mode_ != Mode::Playing && mode_ != Mode::Simulating) return false;
    pausedFrom_ = mode_;
    manager_->pause_play(); // Playing → Pause, Simulating → Pause
    mode_ = Mode::Paused;
    return true;
}

bool EditorRuntimeBridge::resume() {
    if (mode_ != Mode::Paused) return false;
    manager_->pause_play(); // Pause → Play
    mode_ = (pausedFrom_ == Mode::Simulating) ? Mode::Simulating : Mode::Playing;
    return true;
}

bool EditorRuntimeBridge::step_frame(float deltaTime) {
    if (mode_ != Mode::Paused) return false;
    tick_once(deltaTime);
    return true;
}

size_t EditorRuntimeBridge::simulate_ticks(Scene& editorScene, size_t tickCount, float deltaTime) {
    const bool startedFromEdit = (mode_ == Mode::Edit);
    if (startedFromEdit) {
        if (!enter_play(editorScene)) return 0;
    }
    for (size_t i = 0; i < tickCount; ++i) {
        tick_once(deltaTime);
    }
    if (startedFromEdit) exit_play();
    return tickCount;
}

bool EditorRuntimeBridge::exit_play() {
    if (mode_ == Mode::Edit) return false;
    manager_->stop_play();
    mode_ = Mode::Edit;
    ticks_ = 0;
    return true;
}

Scene* EditorRuntimeBridge::play_world() {
    if (mode_ == Mode::Edit) return nullptr;
    return manager_->get_active_scene();
}

const Scene* EditorRuntimeBridge::play_world() const {
    if (mode_ == Mode::Edit) return nullptr;
    return manager_->get_active_scene();
}

void EditorRuntimeBridge::tick_once(float deltaTime) {
    Scene* world = play_world();
    if (!world || !tick_) return;
    tick_(world, deltaTime);
    ++ticks_;
}

bool EditorRuntimeBridge::capture_editor_signature() {
    if (!editorScene_) return false;
    editorSignature_ = scene_signature(*editorScene_);
    return true;
}

bool EditorRuntimeBridge::editor_world_intact() const {
    if (!editorScene_ || editorSignature_.empty()) return true;
    return scene_signature(*editorScene_) == editorSignature_;
}

std::string EditorRuntimeBridge::scene_signature(const Scene& scene) {
    // Deterministic: entity ids are sorted so unordered_map iteration order
    // never affects the signature.
    std::vector<Engine::UUID> ids;
    ids.reserve(scene.get_entities().size());
    for (const auto& [id, entity] : scene.get_entities()) {
        (void)entity;
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());

    std::string signature;
    for (const auto& id : ids) {
        const Entity* entity = scene.find_entity_by_id_const(id);
        if (!entity) continue;
        signature += id.to_string();
        signature += '|';
        signature += entity->get_name();
        signature += '|';
        const auto it = scene.transformComponents.find(id);
        if (it != scene.transformComponents.end()) {
            const auto& t = it->second;
            char buffer[160];
            std::snprintf(buffer, sizeof(buffer),
                          "%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g;",
                          t.position.x, t.position.y, t.position.z,
                          t.rotation.x, t.rotation.y, t.rotation.z,
                          t.scale.x, t.scale.y, t.scale.z);
            signature += buffer;
        }
        signature += '\n';
    }
    return std::to_string(std::hash<std::string>{}(signature));
}

} // namespace Engine::Editor
