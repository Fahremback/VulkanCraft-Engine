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

std::deque<EditorControlApi::PendingCommand> EditorControlApi::drain_commands() {
    std::lock_guard<std::mutex> lock(m_cmdMutex);
    std::deque<PendingCommand> out;
    out.swap(m_commands);
    return out;
}

void EditorControlApi::complete_command(uint64_t id, bool ok, const std::string& message,
                                        const std::string& data) {
    std::lock_guard<std::mutex> lock(m_resultMutex);
    const auto esc = [](const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (const char ch : s) {
            if (ch == '\\' || ch == '"') out.push_back('\\');
            out.push_back(ch);
        }
        return out;
    };
    std::string body;
    if (ok) {
        body = "{\"ok\":true";
        if (!data.empty()) body += ",\"data\":\"" + esc(data) + "\"";
        body += "}";
    } else {
        body = "{\"ok\":false,\"error\":\"" + esc(message) + "\"}";
    }
    m_results[id] = body;
    m_resultCV.notify_all();
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

// Simple HTTP response writer (single request per connection). Status lets the
// agent distinguish a real success (200) from a rejected/unknown command (404)
// or an execution failure (422) instead of trusting a 200 with an error body.
void send_response(SOCKET_TYPE conn, const std::string& body, const char* contentType = "application/json",
                   int status = 200) {
    const char* reason = "OK";
    switch (status) {
        case 400: reason = "Bad Request"; break;
        case 404: reason = "Not Found"; break;
        case 422: reason = "Unprocessable Entity"; break;
        case 500: reason = "Internal Server Error"; break;
        case 504: reason = "Gateway Timeout"; break;
        default: break;
    }
    char header[512];
    std::snprintf(header, sizeof(header),
                  "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                  status, reason, contentType, body.size());
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

// Query value with a leading "key=" token stripped: "?path=assets/x.png"
// (route also matches on the key) must become "assets/x.png", not
// "path=assets/x.png".
std::string query_value_after(const std::string& path, const char* key) {
    std::string v = query_value(path);
    const std::string prefix = std::string(key) + "=";
    if (v.rfind(prefix, 0) == 0) v = v.substr(prefix.size());
    return v;
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

    if (method == "GET" && path == "/panels") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        const auto esc = [](const std::string& in) {
            std::string out;
            out.reserve(in.size() + 8);
            for (const char ch : in) {
                if (ch == '\\' || ch == '"') out.push_back('\\');
                out.push_back(ch);
            }
            return out;
        };
        std::ostringstream out;
        out << "{\"count\":" << snapshot.panels.size() << ",\"panels\":[";
        for (std::size_t i = 0; i < snapshot.panels.size(); ++i) {
            if (i) out << ",";
            out << "\"" << esc(snapshot.panels[i]) << "\"";
        }
        out << "]}";
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/templates") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        const auto esc = [](const std::string& in) {
            std::string out;
            out.reserve(in.size() + 8);
            for (const char ch : in) {
                if (ch == '\\' || ch == '"') out.push_back('\\');
                out.push_back(ch);
            }
            return out;
        };
        std::ostringstream out;
        out << "{\"count\":" << snapshot.templates.size() << ",\"templates\":[";
        for (std::size_t i = 0; i < snapshot.templates.size(); ++i) {
            if (i) out << ",";
            out << "\"" << esc(snapshot.templates[i]) << "\"";
        }
        out << "]}";
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/ui-doc") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        if (snapshot.ui_doc.empty()) {
            send_response(conn, "{\"valid\":false,\"doc\":\"\"}");
            return;
        }
        // The document is already JSON; wrap it so consumers can rely on a
        // stable envelope ({valid, doc}).
        std::ostringstream out;
        out << "{\"valid\":true,\"doc\":" << snapshot.ui_doc << "}";
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/layout") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        if (snapshot.layout.empty()) {
            send_response(conn, "{\"layout\":\"\"}");
            return;
        }
        // The snapshot is already JSON; wrap it in a stable envelope.
        std::ostringstream out;
        out << "{\"layout\":" << snapshot.layout << "}";
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/messages") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        std::ostringstream out;
        if (snapshot.messages.empty()) {
            out << "{\"valid\":false,\"doc\":\"\"}";
        } else {
            out << "{\"valid\":true,\"doc\":" << snapshot.messages << "}";
        }
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/shortcuts") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        const auto esc = [](const std::string& in) {
            std::string out;
            out.reserve(in.size() + 8);
            for (const char ch : in) {
                if (ch == '\\' || ch == '"') out.push_back('\\');
                out.push_back(ch);
            }
            return out;
        };
        std::ostringstream out;
        out << "{\"markdown\":\"" << esc(snapshot.shortcuts) << "\"}";
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/play-mode") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        // The snapshot is already the IPlayMode contract's JSON.
        std::ostringstream out;
        out << "{\"play_mode\":" << snapshot.play_mode << "}";
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/commands/search") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        std::ostringstream out;
        if (snapshot.command_index.empty()) {
            out << "{\"valid\":false,\"index\":\"\"}";
        } else {
            out << "{\"valid\":true,\"index\":" << snapshot.command_index << "}";
        }
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/profiler") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        // The snapshot is already the IFrameProfiler contract's JSON.
        std::ostringstream out;
        if (snapshot.profiler.empty()) {
            out << "{\"valid\":false,\"stats\":\"\"}";
        } else {
            out << "{\"valid\":true,\"stats\":" << snapshot.profiler << "}";
        }
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/undo") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        // The snapshot is already the IUndoHistory contract's JSON.
        std::ostringstream out;
        out << "{\"undo\":" << snapshot.undo << "}";
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/content-browser") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        std::ostringstream out;
        if (snapshot.content_browser.empty()) {
            out << "{\"valid\":false,\"browser\":\"\"}";
        } else {
            out << "{\"valid\":true,\"browser\":" << snapshot.content_browser << "}";
        }
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/window-mode") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        std::ostringstream out;
        if (snapshot.window_mode.empty()) {
            out << "{\"valid\":false,\"window_mode\":\"\"}";
        } else {
            out << "{\"valid\":true,\"window_mode\":" << snapshot.window_mode << "}";
        }
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/camera") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        std::ostringstream out;
        if (snapshot.camera.empty()) {
            out << "{\"valid\":false,\"camera\":\"\"}";
        } else {
            out << "{\"valid\":true,\"camera\":" << snapshot.camera << "}";
        }
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/gizmo") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        std::ostringstream out;
        if (snapshot.gizmo.empty()) {
            out << "{\"valid\":false,\"gizmo\":\"\"}";
        } else {
            out << "{\"valid\":true,\"gizmo\":" << snapshot.gizmo << "}";
        }
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/publish") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        std::ostringstream out;
        if (snapshot.publish.empty()) {
            out << "{\"valid\":false,\"publish\":\"\"}";
        } else {
            out << "{\"valid\":true,\"publish\":" << snapshot.publish << "}";
        }
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/inspector") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        std::ostringstream out;
        if (snapshot.inspector.empty()) {
            out << "{\"valid\":false,\"inspector\":\"\"}";
        } else {
            out << "{\"valid\":true,\"inspector\":" << snapshot.inspector << "}";
        }
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/hierarchy") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        std::ostringstream out;
        if (snapshot.hierarchy.empty()) {
            out << "{\"valid\":false,\"hierarchy\":\"\"}";
        } else {
            out << "{\"valid\":true,\"hierarchy\":" << snapshot.hierarchy << "}";
        }
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/onboarding") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        std::ostringstream out;
        if (snapshot.onboarding.empty()) {
            out << "{\"valid\":false,\"onboarding\":\"\"}";
        } else {
            out << "{\"valid\":true,\"onboarding\":" << snapshot.onboarding << "}";
        }
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/timeline-editor") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        std::ostringstream out;
        if (snapshot.timeline_editor.empty()) {
            out << "{\"valid\":false,\"timeline_editor\":\"\"}";
        } else {
            out << "{\"valid\":true,\"timeline_editor\":" << snapshot.timeline_editor << "}";
        }
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/launcher") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        std::ostringstream out;
        if (snapshot.launcher.empty()) {
            out << "{\"valid\":false,\"launcher\":\"\"}";
        } else {
            out << "{\"valid\":true,\"launcher\":" << snapshot.launcher << "}";
        }
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/retargeting") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        std::ostringstream out;
        if (snapshot.retargeting.empty()) {
            out << "{\"valid\":false,\"retargeting\":\"\"}";
        } else {
            out << "{\"valid\":true,\"retargeting\":" << snapshot.retargeting << "}";
        }
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/qt-doc") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        std::ostringstream out;
        if (snapshot.qt_doc.empty()) {
            out << "{\"valid\":false,\"qt_doc\":\"\"}";
        } else {
            out << "{\"valid\":true,\"qt_doc\":" << snapshot.qt_doc << "}";
        }
        send_response(conn, out.str());
        return;
    }
    if (method == "GET" && path == "/qt-theme") {
        EditorApiState snapshot;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            snapshot = m_live;
        }
        std::ostringstream out;
        if (snapshot.qt_theme.empty()) {
            out << "{\"valid\":false,\"qt_theme\":\"\"}";
        } else {
            out << "{\"valid\":true,\"qt_theme\":" << snapshot.qt_theme << "}";
        }
        send_response(conn, out.str());
        return;
    }
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
        else if (path.rfind("/open-scene?path=", 0) == 0) cmd = "open-scene " + query_value_after(path, "path");
        else if (path.rfind("/add-entity/", 0) == 0) cmd = "add-entity " + slash_args(path, 12);
        else if (path.rfind("/select/", 0) == 0) cmd = "select " + slash_args(path, 8);
        else if (path.rfind("/select?name=", 0) == 0) cmd = "select-name " + query_value_after(path, "name");
        else if (path.rfind("/delete-entity/", 0) == 0) cmd = "delete-entity " + path.substr(15);
        else if (path.rfind("/rename-entity/", 0) == 0) {
            // /rename-entity/{uuid}?name=...
            const size_t q = path.find('?', 15);
            if (q != std::string::npos) {
                cmd = "rename-entity " + path.substr(15, q - 15) + " " + query_value_after(path, "name");
            }
        } else if (path.rfind("/set-transform/", 0) == 0) cmd = "set-transform " + slash_args(path, 15);
        else if (path.rfind("/add-component/", 0) == 0) cmd = "add-component " + slash_args(path, 15);
        else if (path.rfind("/gizmo/", 0) == 0) cmd = "gizmo " + slash_args(path, 7);
        else if (path.rfind("/gizmo-space/", 0) == 0) cmd = "gizmo-space " + slash_args(path, 13);
        else if (path.rfind("/snap/", 0) == 0) cmd = "snap " + slash_args(path, 6);
        // ---- Assets -----------------------------------------------------------
        else if (path.rfind("/import?path=", 0) == 0) cmd = "import " + query_value_after(path, "path");
        else if (path.rfind("/block-model/", 0) == 0) cmd = "block-model " + path.substr(13);
        else if (path.rfind("/block-faces/", 0) == 0) {
            // /block-faces/{blockId}?top=..&side=..&bottom=.. — each param is
            // optional; absent/empty falls back to "0" (keep current face).
            const size_t q = path.find('?', 13);
            const std::string blockId = (q == std::string::npos) ? path.substr(13) : path.substr(13, q - 13);
            const auto qv = [&](const char* key) -> std::string {
                std::string raw = query_value_after(path, key);
                const size_t amp = raw.find('&');
                if (amp != std::string::npos) raw.resize(amp);
                return raw.empty() ? "0" : raw;
            };
            cmd = "block-faces " + blockId + " " + qv("top") + " " + qv("side") + " " + qv("bottom");
        }
        else if (path.rfind("/block-model-faces/", 0) == 0) {
            // /block-model-faces/{base}/{top}/{side}/{bottom}?name=..
            const size_t q = path.find('?');
            const std::string pathPart = (q == std::string::npos) ? path.substr(19) : path.substr(19, q - 19);
            std::string name = query_value_after(path, "name");
            const size_t amp = name.find('&');
            if (amp != std::string::npos) name.resize(amp);
            // pathPart is "base/top/side/bottom" but the command parser splits
            // on whitespace, so normalize the separators before dispatching.
            std::string faces = pathPart;
            for (char& c : faces) { if (c == '/') c = ' '; }
            cmd = "block-model-faces " + faces + " " + name;
        }
        else if (path.rfind("/spawn-block/", 0) == 0) cmd = "spawn-block " + path.substr(13);
        else if (path.rfind("/spawn-character/", 0) == 0) cmd = "spawn-character " + path.substr(17);
        // ---- Runtime-wired Wicked-port features (Layers/Decal/Hair/SoftBody/
        //      EnvProbe/Paint/Video/Gaussian/Expressions) ----------------------
        else if (path.rfind("/layer/", 0) == 0) cmd = "layer " + path.substr(7);
        else if (path.rfind("/layer-vis?name=", 0) == 0) {
            // /layer-vis?name=<url-encoded>&visible=0 — parse each key: the
            // generic query_value_after returns the whole tail after the first
            // key, so split on '&' manually.
            const size_t q = path.find('?');
            const std::string query = (q == std::string::npos) ? "" : path.substr(q + 1);
            const size_t amp = query.find('&');
            const std::string namePart = (amp == std::string::npos) ? query : query.substr(0, amp);
            const std::string visPart = (amp == std::string::npos) ? "" : query.substr(amp + 1);
            std::string name = namePart.rfind("name=", 0) == 0 ? namePart.substr(5) : namePart;
            std::string vis = "1";
            if (visPart.rfind("visible=", 0) == 0) vis = visPart.substr(8);
            cmd = "layer-vis " + url_decode(name) + "|" + vis;
        }
        else if (path.rfind("/decal-add/", 0) == 0) {
            const size_t q = path.find('?');
            const std::string uuid = (q == std::string::npos) ? path.substr(11) : path.substr(11, q - 11);
            cmd = "decal-add " + uuid + " " + query_value_after(path, "texture");
        }
        else if (path.rfind("/hair-add/", 0) == 0) cmd = "hair-add " + path.substr(10);
        else if (path.rfind("/softbody-add/", 0) == 0) cmd = "softbody-add " + path.substr(14);
        else if (path.rfind("/env-add/", 0) == 0) cmd = "env-add " + path.substr(9);
        else if (path.rfind("/env-capture/", 0) == 0) cmd = "env-capture " + path.substr(13);
        else if (path.rfind("/paint-add/", 0) == 0) cmd = "paint-add " + path.substr(11);
        else if (path.rfind("/paint-mode/", 0) == 0) cmd = "paint-mode " + slash_args(path, 12);
        else if (path.rfind("/paint-color/", 0) == 0) cmd = "paint-color " + slash_args(path, 13);
        else if (path.rfind("/video-add/", 0) == 0) cmd = "video-add " + path.substr(11);
        else if (path.rfind("/video-frame/", 0) == 0) {
            const size_t q = path.find('?');
            const std::string uuid = (q == std::string::npos) ? path.substr(13) : path.substr(13, q - 13);
            cmd = "video-frame " + uuid + " " + query_value_after(path, "name");
        }
        else if (path.rfind("/video-play/", 0) == 0) cmd = "video-play " + slash_args(path, 12);
        else if (path.rfind("/gaussian-add/", 0) == 0) cmd = "gaussian-add " + path.substr(13);
        else if (path.rfind("/gaussian-regen/", 0) == 0) cmd = "gaussian-regen " + path.substr(15);
        else if (path.rfind("/expression-add/", 0) == 0) cmd = "expression-add " + slash_args(path, 15);
        else if (path.rfind("/asset-duplicate/", 0) == 0) cmd = "asset-duplicate " + path.substr(17);
        else if (path.rfind("/asset-delete/", 0) == 0) cmd = "asset-delete " + path.substr(14);
        else if (path.rfind("/reimport/", 0) == 0) cmd = "reimport " + path.substr(10);
        else if (path.rfind("/import-pack?path=", 0) == 0) cmd = "import-pack " + query_value_after(path, "path");
        // ---- Voxel ------------------------------------------------------------
        else if (path.rfind("/voxel-generate/", 0) == 0) cmd = "voxel-generate " + slash_args(path, 16);
        else if (path.rfind("/voxel-clear/", 0) == 0) cmd = "voxel-clear " + path.substr(13);
        else if (path.rfind("/voxel-paint/", 0) == 0) cmd = "voxel-paint " + slash_args(path, 13);
        else if (path.rfind("/voxel-block/", 0) == 0) cmd = "voxel-block " + slash_args(path, 13);
        // ---- Scripts ----------------------------------------------------------
        else if (path.rfind("/script-event?name=", 0) == 0) cmd = "script-event " + query_value_after(path, "name");
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
        else if (path.rfind("/screenshot-ui", 0) == 0) {
            // /screenshot-ui?path=... — captures the FINAL FRAME (viewport + UI
            // ImGui/docks) from the UI snapshot; validar o visual por algoritmo.
            // Deve vir ANTES do /screenshot (prefixo tambem casa /screenshot-ui).
            const std::string p = query_value_after(path, "path");
            cmd = p.empty() ? "screenshot-ui" : "screenshot-ui " + p;
        }
        else if (path.rfind("/screenshot", 0) == 0) {
            // /screenshot?path=... — captures the viewport to a PNG and returns
            // the saved path in `data` so an agent can see the result.
            const std::string p = query_value_after(path, "path");
            cmd = p.empty() ? "screenshot" : "screenshot " + p;
        }
        if (!cmd.empty()) {
            // Queue the command, then WAIT for the editor main thread to
            // actually execute it and report the real outcome. This replaces
            // the old fire-and-forget `{"ok":true}` that lied to the agent
            // when a command failed after the fact.
            const uint64_t id = m_nextCmdId.fetch_add(1);
            {
                std::lock_guard<std::mutex> lock(m_cmdMutex);
                m_commands.push_back({ id, cmd });
            }
            std::string body;
            int status = 200;
            {
                std::unique_lock<std::mutex> lk(m_resultMutex);
                const bool done = m_resultCV.wait_for(
                    lk, std::chrono::seconds(15),
                    [&] { return m_results.count(id) != 0; });
                if (done) {
                    body = m_results[id];
                    status = (body.find("\"ok\":true") != std::string::npos) ? 200 : 422;
                    m_results.erase(id);
                } else {
                    body = "{\"ok\":false,\"error\":\"timeout: the editor did not execute the command\"}";
                    status = 504;
                }
            }
            send_response(conn, body, "application/json", status);
            return;
        }
    }
    // Unknown endpoint: a real 404, not a 200 with an error body.
    send_response(conn, "{\"error\":\"unknown endpoint\"}", "application/json", 404);
}

} // namespace Engine
