#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Engine {

// Minimal loopback HTTP control API for the editor (127.0.0.1:<port>).
//
// Lets a terminal, an automation agent or an MCP client drive the editor with
// plain commands instead of synthetic mouse clicks:
//
//   POST /play    start the in-engine game (Play In Editor)
//   POST /pause   freeze the simulation (Play → Pause)
//   POST /resume  resume from the freeze  (Pause → Play)
//   POST /stop    exit the game back to Edit
//   POST /step    advance exactly 1 frame while paused
//   GET  /state   JSON: {"state":"edit|play|pause|simulate","fps":..,"entities":..}
//   GET  /health  "ok"
//
// Threading: the HTTP server runs on its own thread; commands are queued and
// drained by the editor main thread each frame, so ImGui/play-state is never
// touched from the server thread. Live state is published by the editor.
// Snapshot the editor publishes each frame for GET /state.
struct EditorApiState {
    std::string state{ "edit" };
    float fps{ 0.0f };
    std::size_t entities{ 0 };
    float orbitDistance{ 10.0f };
    bool viewportHovered{ false };
    bool imageHovered{ false };
    bool keyboardCapture{ false };
    bool typing{ false };
    float camX{ 0.0f }, camY{ 0.0f }, camZ{ 0.0f };
    float yaw{ 0.0f }, pitch{ 0.0f };
    // Scene/editor state (selection, gizmo, snap, camera target).
    std::string selectedEntity;
    std::string gizmoMode{ "translate" };
    float snap{ 1.0f };
    float camTargetX{ 0.0f }, camTargetY{ 0.0f }, camTargetZ{ 0.0f };
    // Panel features (Opções Gráficas / Terreno / Configurações) telemetry.
    bool vsync{ true };
    int shadowQuality{ 2 };
    bool terrainValid{ false };
    std::size_t terrainVertices{ 0 };
    std::size_t terrainTriangles{ 0 };
    bool meshEdited{ false };
    std::string settingsPath;
    // Dev telemetry: last headless self-test result, script VM state.
    std::string lastSelfTest;
    std::string scriptState;
    // Registered editor panels (EditorPluginRegistry), insertion order — the
    // shell's menu/palette/layout surface, exposed for CLI/MCP observability.
    std::vector<std::string> panels;
    // Project templates (ProjectTemplateRegistry), insertion order — the
    // project wizard's catalog, exposed for CLI/MCP observability.
    std::vector<std::string> templates;
};

class EditorControlApi {
public:
    EditorControlApi() = default;
    ~EditorControlApi();
    EditorControlApi(const EditorControlApi&) = delete;
    EditorControlApi& operator=(const EditorControlApi&) = delete;

    bool start(uint16_t port = 8321);
    void stop();

    // A queued command carries an id so the HTTP thread can wait for the
    // editor main thread to actually execute it and report the real result
    // (no more fire-and-forget `{"ok":true}` before anything ran).
    struct PendingCommand {
        uint64_t id;
        std::string cmd;
    };

    // Editor main thread: pull queued commands (executed in main_loop).
    std::deque<PendingCommand> drain_commands();

    // Editor main thread: report the real outcome of command `id` so the
    // waiting HTTP request can answer with success/failure + message. `data`
    // is optional free-form JSON payload (e.g. a screenshot path) embedded as
    // `"data":"..."` on success.
    void complete_command(uint64_t id, bool ok, const std::string& message,
                          const std::string& data = std::string());

    // Editor main thread: publish live state for GET /state.
    void publish_state(const EditorApiState& s);

private:
    void server_loop(uint16_t port);
    void handle_request(uint64_t conn, const std::string& rawRequest);

    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    std::atomic<uint64_t> m_listenSocket{ 0 };

    std::mutex m_cmdMutex;
    std::deque<PendingCommand> m_commands;

    std::mutex m_resultMutex;
    std::condition_variable m_resultCV;
    std::unordered_map<uint64_t, std::string> m_results;
    std::atomic<uint64_t> m_nextCmdId{ 1 };

    std::mutex m_stateMutex;
    EditorApiState m_live;
};

} // namespace Engine
