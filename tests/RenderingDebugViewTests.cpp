// RenderingDebugViewTests.cpp — Agente 1 (task_plan A.17): headless gate for
// the PUBLIC debug-view data model (IRenderingDebugView). Proves the snapshot
// aggregation and the deterministic JSON emitter (bit-exact, key order) — no
// GPU.

#include "engine/rendering/IRenderingDebugView.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

void check(bool condition, const std::string& message) {
    check(condition, message.c_str());
}

using Engine::Rendering::DebugCard;
using Engine::Rendering::DebugProbe;
using Engine::Rendering::DebugTracePath;
using Engine::Rendering::IRenderingDebugView;
using Engine::Rendering::create_rendering_debug_view;

}  // namespace

int main() {
    using namespace Engine::Rendering;

    // ---- 1. empty snapshot serializes to a canonical empty object ----
    {
        std::string error;
        auto view = create_rendering_debug_view(error);
        check(view != nullptr && error.empty(), "debug view created");
        const std::string json = view->to_json();
        check(json.find("\"cardCount\":0") != std::string::npos,
              "empty snapshot has cardCount 0");
        check(json.find("\"probes\":[]") != std::string::npos, "empty probes array");
        check(json.find("\"tracePaths\":[]") != std::string::npos,
              "empty trace paths array");
    }

    // ---- 2. bind cards + capture + probes + trace + disocclusion ----
    {
        std::string error;
        auto view = create_rendering_debug_view(error);

        DebugCard card;
        card.center = glm::vec3(1.0f, 0.5f, 2.0f);
        card.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        card.albedo = glm::vec4(0.3f, 0.5f, 0.2f, 1.0f);
        card.emissive = glm::vec3(0.0f);
        card.cascade = 0;
        view->bind_cards({ card }, { 1 });

        view->bind_capture(1, 3, 64);

        DebugProbe probe;
        probe.radianceVisibility = glm::vec4(0.25f, 0.28f, 0.32f, 1.0f);
        probe.worldCellCascade = glm::ivec4(0, 0, 0, 0);
        view->bind_probes({ probe }, 0, 2);

        DebugTracePath path;
        path.origin = glm::vec3(0.0f);
        path.direction = glm::vec3(0.0f, 0.0f, -1.0f);
        path.hit = true;
        path.distance = 5.0f;
        path.steps = 4;
        view->add_trace_path(path);

        view->bind_disocclusion(7, 3);
        view->refresh();

        const auto& snap = view->snapshot();
        check(snap.cardCount == 1, "snapshot card count == 1");
        check(snap.cardsPerCascade.size() == 1 && snap.cardsPerCascade[0] == 1,
              "cardsPerCascade == [1]");
        check(snap.capturedCount == 1 && snap.pendingCount == 3 && snap.vramBytes == 64,
              "capture counters bound");
        check(snap.probeCount == 1 && snap.sunRevision == 2,
              "probe + sun revision bound");
        check(snap.tracePaths.size() == 1 && snap.tracePaths[0].hit,
              "trace path bound");
        check(snap.disoccludedPixels == 7 && snap.confidenceLevel == 3,
              "disocclusion + confidence bound");
    }

    // ---- 3. JSON round-trip is deterministic (bit-exact cross-instance) ----
    {
        std::string error;
        auto a = create_rendering_debug_view(error);
        auto b = create_rendering_debug_view(error);

        DebugCard card;
        card.center = glm::vec3(1.0f, 0.5f, 2.0f);
        card.normal = glm::vec3(0.0f, 1.0f, 0.0f);
        card.albedo = glm::vec4(0.3f, 0.5f, 0.2f, 1.0f);
        card.cascade = 1;
        a->bind_cards({ card }, { 0, 1 });
        b->bind_cards({ card }, { 0, 1 });

        DebugTracePath path;
        path.origin = glm::vec3(1.0f, 2.0f, 3.0f);
        path.direction = glm::vec3(0.0f, -1.0f, 0.0f);
        path.hit = false;
        path.distance = 7.5f;
        path.steps = 12;
        a->add_trace_path(path);
        b->add_trace_path(path);

        a->bind_disocclusion(2, 1);
        b->bind_disocclusion(2, 1);

        check(a->to_json() == b->to_json(),
              "two views serialize bit-identical JSON (determinism)");
    }

    if (g_failures == 0) {
        std::printf("[rendering-debug-view] ALL PASSED\n");
        return 0;
    }
    std::printf("[rendering-debug-view] %d FAILURE(S)\n", g_failures);
    return 1;
}