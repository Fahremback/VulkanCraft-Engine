#include "EditorControlApi.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define CLOSE_SOCKET closesocket
#define SOCKET_TYPE SOCKET
#define INVALID_SOCKET_TYPE INVALID_SOCKET
#else
// POSIX fallback (kept compiling for non-Windows builds; the editor is Win32).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE_SOCKET close
#define SOCKET_TYPE int
#define INVALID_SOCKET_TYPE (-1)
typedef int SOCKLEN_T;
#endif

#include <cstdio>
#include <cstring>
#include <sstream>

namespace Engine {

EditorControlApi::~EditorControlApi() {
    stop();
}

bool EditorControlApi::start(uint16_t port) {
    if (m_running.exchange(true)) return true; // already running
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "[ControlApi] WSAStartup failed\n");
        m_running = false;
        return false;
    }
#endif
    m_thread = std::thread([this, port] { server_loop(port); });
    return true;
}

void EditorControlApi::stop() {
    if (!m_running.exchange(false)) return;
    const SOCKET_TYPE s = static_cast<SOCKET_TYPE>(m_listenSocket.load());
    if (s != INVALID_SOCKET_TYPE) {
        CLOSE_SOCKET(s); // unblocks accept() in the server thread
    }
    if (m_thread.joinable()) m_thread.join();
#ifdef _WIN32
    WSACleanup();
#endif
}

std::deque<std::string> EditorControlApi::drain_commands() {
    std::lock_guard<std::mutex> lock(m_cmdMutex);
    std::deque<std::string> out;
    out.swap(m_commands);
    return out;
}

void EditorControlApi::publish_state(const EditorApiState& s) {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_live = s;
}

void EditorControlApi::server_loop(uint16_t port) {
    SOCKET_TYPE listenSock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET_TYPE) {
        std::fprintf(stderr, "[ControlApi] socket() failed\n");
        m_running = false;
        return;
    }
    int yes = 1;
    ::setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // loopback only — never exposed
    addr.sin_port = htons(port);
    if (::bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "[ControlApi] bind 127.0.0.1:%u failed\n", port);
        CLOSE_SOCKET(listenSock);
        m_running = false;
        return;
    }
    if (::listen(listenSock, 8) != 0) {
        std::fprintf(stderr, "[ControlApi] listen failed\n");
        CLOSE_SOCKET(listenSock);
        m_running = false;
        return;
    }
    m_listenSocket.store(static_cast<uint64_t>(listenSock));
    std::printf("[ControlApi] listening on http://127.0.0.1:%u/  (play/pause/resume/stop/step/state)\n", port);

    while (m_running.load()) {
        sockaddr_in client{};
#ifdef _WIN32
        int clientLen = static_cast<int>(sizeof(client));
#else
        socklen_t clientLen = sizeof(client);
#endif
        SOCKET_TYPE conn = ::accept(listenSock, reinterpret_cast<sockaddr*>(&client), &clientLen);
        if (conn == INVALID_SOCKET_TYPE) break; // listen socket closed → shutting down
        char buf[4096];
        int n = static_cast<int>(::recv(conn, buf, sizeof(buf) - 1, 0));
        if (n > 0) {
            buf[n] = '\0';
            handle_request(static_cast<uint64_t>(conn), std::string(buf, static_cast<size_t>(n)));
        }
        CLOSE_SOCKET(conn);
    }
    CLOSE_SOCKET(listenSock);
    m_running = false;
}

namespace {

// Simple "HTTP/1.1 200 OK" response writer (single request per connection).
void send_response(SOCKET_TYPE conn, const std::string& body, const char* contentType = "application/json") {
    char header[512];
    std::snprintf(header, sizeof(header),
                  "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                  contentType, body.size());
    ::send(conn, header, static_cast<int>(std::strlen(header)), 0);
    ::send(conn, body.data(), static_cast<int>(body.size()), 0);
}

// Percent-decodes a query string value (%20 -> space, '+' -> space).
std::string url_decode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') { out.push_back(' '); continue; }
        if (in[i] == '%' && i + 2 < in.size()) {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

// Splits "/path?query" into the path and the (url-decoded) query value.
std::string query_value(const std::string& path) {
    const size_t q = path.find('?');
    if (q == std::string::npos) return std::string();
    return url_decode(path.substr(q + 1));
}

// Takes the substring after "/prefix/" and turns '/' separators into spaces.
std::string slash_args(const std::string& path, size_t prefixLen) {
    std::string rest = path.substr(prefixLen);
    for (char& c : rest) if (c == '/') c = ' ';
    return rest;
}

} // namespace

void EditorControlApi::handle_request(uint64_t connRaw, const std::string& rawRequest) {
    const SOCKET_TYPE conn = static_cast<SOCKET_TYPE>(connRaw);
    // Parse the request line: "METHOD /path HTTP/1.1".
    std::istringstream in(rawRequest);
    std::string method, path;
    in >> method >> path;
    if (path.empty()) path = "/";

    if (method == "GET" && path == "/health") {
        send_response(conn, "ok", "text/plain");
        return;
    }
    if (method == "GET" && path == "/state") {
        EditorApiState s;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            s = m_live;
        }
        // Windows paths contain single backslashes which are invalid JSON
        // escapes; always escape text fields before embedding them.
        const auto esc = [](const std::string& in) {
            std::string out;
            out.reserve(in.size() + 8);
            for (const char ch : in) {
                if (ch == '\\' || ch == '"') out.push_back('\\');
                out.push_back(ch);
            }
            return out;
        };
        const std::string settingsPathJson = esc(s.settingsPath);
        const std::string selectedJson = esc(s.selectedEntity);
        const std::string gizmoJson = esc(s.gizmoMode);
        const std::string selfTestJson = esc(s.lastSelfTest);
        const std::string scriptJson = esc(s.scriptState);
        char body[4096];
        std::snprintf(body, sizeof(body),
                      "{\"state\":\"%s\",\"fps\":%.1f,\"entities\":%zu,\"orbitDistance\":%.3f,"
                      "\"viewportHovered\":%s,\"imageHovered\":%s,\"keyboardCapture\":%s,\"typing\":%s,"
                      "\"camera\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"yaw\":%.2f,\"pitch\":%.2f,"
                      "\"cameraTarget\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},"
                      "\"vsync\":%s,\"shadowQuality\":%d,\"terrainValid\":%s,"
                      "\"terrainVertices\":%zu,\"terrainTriangles\":%zu,\"meshEdited\":%s,"
                      "\"settingsPath\":\"%s\",\"selectedEntity\":\"%s\","
                      "\"gizmoMode\":\"%s\",\"snap\":%.3f,\"lastSelfTest\":\"%s\",\"scriptState\":\"%s\"}",
                      s.state.c_str(), s.fps, s.entities, s.orbitDistance,
                      s.viewportHovered ? "true" : "false", s.imageHovered ? "true" : "false",
                      s.keyboardCapture ? "true" : "false", s.typing ? "true" : "false",
                      s.camX, s.camY, s.camZ, s.yaw, s.pitch,
                      s.camTargetX, s.camTargetY, s.camTargetZ,
                      s.vsync ? "true" : "false", s.shadowQuality,
                      s.terrainValid ? "true" : "false", s.terrainVertices, s.terrainTriangles,
                      s.meshEdited ? "true" : "false", settingsPathJson.c_str(),
                      selectedJson.c_str(), gizmoJson.c_str(), s.snap,
                      selfTestJson.c_str(), scriptJson.c_str());
        send_response(conn, body);
        return;
    }
    if (method == "POST") {
        std::string cmd;
        // ---- Play / simulation -------------------------------------------------
        if (path == "/play") cmd = "play";
        else if (path == "/pause") cmd = "pause";
        else if (path == "/resume") cmd = "resume";
        else if (path == "/stop") cmd = "stop";
        else if (path == "/step") cmd = "step";
        else if (path == "/simulate") cmd = "simulate";
        // ---- Camera -----------------------------------------------------------
        else if (path.rfind("/zoom/", 0) == 0) cmd = "zoom " + slash_args(path, 6);
        else if (path.rfind("/move/", 0) == 0) cmd = "move " + slash_args(path, 6);
        else if (path.rfind("/turn/", 0) == 0) cmd = "turn " + slash_args(path, 6);
        else if (path.rfind("/focus/", 0) == 0) cmd = "focus " + slash_args(path, 7);
        // ---- Scene ------------------------------------------------------------
        else if (path == "/new-scene") cmd = "new-scene";
        else if (path == "/save-scene") cmd = "save-scene";
        else if (path.rfind("/open-scene?path=", 0) == 0) cmd = "open-scene " + query_value(path);
        else if (path.rfind("/add-entity/", 0) == 0) cmd = "add-entity " + slash_args(path, 12);
        else if (path.rfind("/select/", 0) == 0) cmd = "select " + slash_args(path, 8);
        else if (path.rfind("/select?name=", 0) == 0) cmd = "select-name " + query_value(path);
        else if (path.rfind("/delete-entity/", 0) == 0) cmd = "delete-entity " + path.substr(15);
        else if (path.rfind("/rename-entity/", 0) == 0) {
            // /rename-entity/{uuid}?name=...
            const size_t q = path.find('?', 15);
            if (q != std::string::npos) {
                cmd = "rename-entity " + path.substr(15, q - 15) + " " + query_value(path);
            }
        } else if (path.rfind("/set-transform/", 0) == 0) cmd = "set-transform " + slash_args(path, 15);
        else if (path.rfind("/add-component/", 0) == 0) cmd = "add-component " + slash_args(path, 15);
        else if (path.rfind("/gizmo/", 0) == 0) cmd = "gizmo " + slash_args(path, 7);
        else if (path.rfind("/gizmo-space/", 0) == 0) cmd = "gizmo-space " + slash_args(path, 13);
        else if (path.rfind("/snap/", 0) == 0) cmd = "snap " + slash_args(path, 6);
        // ---- Assets -----------------------------------------------------------
        else if (path.rfind("/import?path=", 0) == 0) cmd = "import " + query_value(path);
        else if (path.rfind("/block-model/", 0) == 0) cmd = "block-model " + path.substr(13);
        else if (path.rfind("/spawn-block/", 0) == 0) cmd = "spawn-block " + path.substr(13);
        else if (path.rfind("/asset-duplicate/", 0) == 0) cmd = "asset-duplicate " + path.substr(17);
        else if (path.rfind("/asset-delete/", 0) == 0) cmd = "asset-delete " + path.substr(14);
        else if (path.rfind("/reimport/", 0) == 0) cmd = "reimport " + path.substr(10);
        // ---- Voxel ------------------------------------------------------------
        else if (path.rfind("/voxel-generate/", 0) == 0) cmd = "voxel-generate " + slash_args(path, 16);
        else if (path.rfind("/voxel-clear/", 0) == 0) cmd = "voxel-clear " + path.substr(13);
        else if (path.rfind("/voxel-paint/", 0) == 0) cmd = "voxel-paint " + slash_args(path, 13);
        // ---- Scripts ----------------------------------------------------------
        else if (path.rfind("/script-event?name=", 0) == 0) cmd = "script-event " + query_value(path);
        else if (path == "/script-pause") cmd = "script-pause";
        else if (path == "/script-continue") cmd = "script-continue";
        else if (path == "/script-step") cmd = "script-step";
        // ---- Windows / tools / theme ------------------------------------------
        else if (path.rfind("/window/", 0) == 0) cmd = "window " + slash_args(path, 8);
        else if (path.rfind("/editor/", 0) == 0) cmd = "editor " + slash_args(path, 8);
        else if (path.rfind("/theme/", 0) == 0) cmd = "theme " + slash_args(path, 7);
        else if (path.rfind("/weather/", 0) == 0) cmd = "weather " + slash_args(path, 9);
        // ---- Terrain / graphics / settings (kept) -----------------------------
        else if (path.rfind("/terrain/", 0) == 0) cmd = "terrain " + slash_args(path, 9);
        else if (path.rfind("/graphics/", 0) == 0) cmd = "graphics " + slash_args(path, 10);
        else if (path == "/save-settings") cmd = "save-settings";
        else if (path.rfind("/project/", 0) == 0) cmd = "project " + path.substr(9);
        else if (path.rfind("/mesh/", 0) == 0) cmd = "mesh " + path.substr(6);
        // ---- Dev --------------------------------------------------------------
        else if (path.rfind("/selftest/", 0) == 0) cmd = "selftest " + path.substr(10);
        else if (path == "/package") cmd = "package";
        else if (path == "/hot-reload") cmd = "hot-reload";
        if (!cmd.empty()) {
            {
                std::lock_guard<std::mutex> lock(m_cmdMutex);
                m_commands.push_back(cmd);
            }
            send_response(conn, "{\"ok\":true}", "text/plain");
            return;
        }
    }
    send_response(conn, "{\"error\":\"unknown endpoint\"}", "text/plain");
}

} // namespace Engine
