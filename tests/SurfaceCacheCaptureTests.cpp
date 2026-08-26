// SurfaceCacheCaptureTests.cpp — Agente 1 (task_plan A.4): headless gate for
// the PUBLIC material-card capture (ISurfaceCacheCapture). Proves capture
// (per-frame card budget, nearest-camera-first), LOCALIZED invalidation,
// automatic re-capture on revision bump, self-luminous handling, exact VRAM
// accounting and determinism — no GPU.

#include "engine/rendering/ILumenScene.hpp"
#include "engine/rendering/ISurfaceCacheCapture.hpp"

#include <cmath>
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

bool near(float a, float b, float eps = 1.0e-3f) {
    return std::fabs(a - b) <= eps;
}

using Engine::Rendering::CaptureConfig;
using Engine::Rendering::CapturedCard;
using Engine::Rendering::ISurfaceCacheCapture;
using Engine::Rendering::ILumenScene;
using Engine::Rendering::LumenSceneConfig;
using Engine::Rendering::LumenSurface;
using Engine::Rendering::RadianceSampler;
using Engine::Rendering::create_lumen_scene;
using Engine::Rendering::create_surface_cache_capture;
using Engine::Rendering::kCapturedCardVramBytes;

LumenSurface ground(float x, float z, float halfExtent = 1.0f) {
    LumenSurface s;
    s.center = glm::vec3(x, 0.5f, z);
    s.normal = glm::vec3(0.0f, 1.0f, 0.0f);
    s.halfExtent = glm::vec2(halfExtent);
    s.albedo = glm::vec4(0.3f, 0.5f, 0.2f, 1.0f);
    s.emissive = glm::vec3(0.0f);
    return s;
}

// Deterministic synthetic radiance: varies with position + normal.
glm::vec4 sample_radiance(const glm::vec3& p, const glm::vec3& n) {
    return glm::vec4(0.1f + p.x * 0.01f, 0.2f, 0.3f, std::max(0.0f, n.y));
}

}  // namespace

int main() {
    using namespace Engine::Rendering;

    // ---- 1. configure all-or-nothing ----
    {
        std::string error;
        auto cap = create_surface_cache_capture(error);
        check(cap != nullptr && error.empty(), "capture created");

        CaptureConfig bad = cap->config();
        bad.cardsPerFrame = 0;
        check(!cap->configure(bad, error) && !error.empty(),
              "cardsPerFrame 0 refused");
        bad = cap->config();
        bad.maxCapturedCards = 0;
        check(!cap->configure(bad, error), "maxCapturedCards 0 refused");
    }

    // ---- 2. full capture + exact vram ----
    {
        std::string error;
        auto scene = create_lumen_scene(error);
        LumenSceneConfig sc = scene->config();
        sc.maxCards = 1000;
        sc.maxCardsPerChunk = 100;
        scene->configure(sc, error);
        scene->replace_chunk(1, 1, { ground(0.0f, 0.0f), ground(10.0f, 0.0f) }, error);
        scene->replace_chunk(2, 1, { ground(0.0f, 20.0f), ground(10.0f, 20.0f) }, error);

        auto cap = create_surface_cache_capture(error);
        cap->bind_scene(scene.get());
        cap->bind_radiance(RadianceSampler(&sample_radiance));

        check(cap->captured_count() == 0 && cap->pending_count() == 4,
              "4 cards pending before capture");
        const std::uint32_t captured = cap->update(glm::vec3(0.0f));
        check(captured == 4, "one update captures all 4 cards");
        check(cap->captured_count() == 4 && cap->pending_count() == 0,
              "fully captured");
        check(cap->vram_bytes() == 4 * kCapturedCardVramBytes,
              "exact vram accounting");
    }

    // ---- 3. per-frame card budget ----
    {
        std::string error;
        auto scene = create_lumen_scene(error);
        LumenSceneConfig sc = scene->config();
        sc.maxCards = 1000;
        sc.maxCardsPerChunk = 100;
        scene->configure(sc, error);
        scene->replace_chunk(1, 1,
            { ground(0.0f, 0.0f), ground(10.0f, 0.0f), ground(20.0f, 0.0f), ground(30.0f, 0.0f) },
            error);

        auto cap = create_surface_cache_capture(error);
        CaptureConfig cc = cap->config();
        cc.cardsPerFrame = 2;
        cap->configure(cc, error);
        cap->bind_scene(scene.get());
        cap->bind_radiance(RadianceSampler(&sample_radiance));

        check(cap->update(glm::vec3(0.0f)) == 2, "frame 1 captures the 2-card budget");
        check(cap->captured_count() == 2 && cap->pending_count() == 2,
              "half captured after frame 1");
        check(cap->update(glm::vec3(0.0f)) == 2, "frame 2 captures the rest");
        check(cap->pending_count() == 0, "drained after frame 2");
        check(cap->update(glm::vec3(0.0f)) == 0, "idempotent: no pending -> 0 captured");
    }

    // ---- 4. self-luminous cards bypass the radiance sampler ----
    {
        std::string error;
        auto scene = create_lumen_scene(error);
        LumenSurface lamp = ground(0.0f, 0.0f);
        lamp.emissive = glm::vec3(1.0f, 0.5f, 0.2f);
        scene->replace_chunk(1, 1, { lamp }, error);

        auto cap = create_surface_cache_capture(error);
        cap->bind_scene(scene.get());
        cap->bind_radiance(RadianceSampler(&sample_radiance));
        cap->update(glm::vec3(0.0f));

        CapturedCard out{};
        check(cap->captured(0, out), "lamp captured");
        check(out.selfLuminous, "emissive card flagged self-luminous");
        check(near(out.irradiance.r, 1.0f) && near(out.irradiance.g, 0.5f) &&
                  near(out.irradiance.b, 0.2f),
              "lamp irradiance == emissive");
        check(near(out.bouncedRadiance.r, 1.0f),
              "lamp bounced radiance == emissive");
    }

    // ---- 5. LOCALIZED invalidation: only the target chunk re-captures ----
    {
        std::string error;
        auto scene = create_lumen_scene(error);
        LumenSceneConfig sc = scene->config();
        sc.maxCards = 1000;
        sc.maxCardsPerChunk = 100;
        scene->configure(sc, error);
        scene->replace_chunk(1, 1, { ground(0.0f, 0.0f) }, error);
        scene->replace_chunk(2, 1, { ground(50.0f, 0.0f) }, error);

        auto cap = create_surface_cache_capture(error);
        cap->bind_scene(scene.get());
        cap->bind_radiance(RadianceSampler(&sample_radiance));
        cap->update(glm::vec3(0.0f));

        // Locate cards by chunk id (capture order follows scene MRU order).
        const auto find_card = [&cap](std::uint64_t id, CapturedCard& out) {
            for (std::uint32_t i = 0; i < cap->captured_count(); ++i) {
                CapturedCard c{};
                if (cap->captured(i, c) && c.card.chunkId == id) {
                    out = c;
                    return true;
                }
            }
            return false;
        };
        CapturedCard a0{}, b0{};
        check(find_card(1, a0) && find_card(2, b0), "both chunks captured");
        const std::uint64_t ageA = a0.captureAge;
        const std::uint64_t ageB = b0.captureAge;

        check(cap->invalidate_chunk(1), "invalidate chunk 1 (exists)");
        check(cap->captured_count() == 1 && cap->pending_count() == 1,
              "only chunk 1 pending after invalidation");

        cap->update(glm::vec3(0.0f));
        CapturedCard a1{}, b1{};
        check(find_card(1, a1) && find_card(2, b1), "both chunks re-captured");
        check(a1.captureAge > ageA, "chunk 1 re-captured with a newer age");
        check(b1.captureAge == ageB, "chunk 2 radiance UNCHANGED (localized)");

        check(!cap->invalidate_chunk(999), "invalidate unknown chunk is a no-op");
    }

    // ---- 6. revision bump re-captures automatically ----
    {
        std::string error;
        auto scene = create_lumen_scene(error);
        LumenSceneConfig sc = scene->config();
        sc.maxCards = 1000;
        sc.maxCardsPerChunk = 100;
        scene->configure(sc, error);
        scene->replace_chunk(1, 1, { ground(0.0f, 0.0f) }, error);

        auto cap = create_surface_cache_capture(error);
        cap->bind_scene(scene.get());
        cap->bind_radiance(RadianceSampler(&sample_radiance));
        cap->update(glm::vec3(0.0f));
        CapturedCard before{};
        cap->captured(0, before);

        scene->replace_chunk(1, 2, { ground(5.0f, 0.0f) }, error);
        check(cap->pending_count() == 0, "stale until update() reconciles");
        cap->update(glm::vec3(0.0f));
        CapturedCard after{};
        cap->captured(0, after);
        check(after.card.revision == 2, "re-captured card carries the new revision");
        check(after.captureAge > before.captureAge, "re-captured with a newer age");
    }

    // ---- 7. nearest-camera-first priority ----
    {
        std::string error;
        auto scene = create_lumen_scene(error);
        LumenSceneConfig sc = scene->config();
        sc.maxCards = 1000;
        sc.maxCardsPerChunk = 100;
        scene->configure(sc, error);
        scene->replace_chunk(1, 1, { ground(100.0f, 0.0f) }, error); // far
        scene->replace_chunk(2, 1, { ground(0.0f, 0.0f) }, error);   // near

        auto cap = create_surface_cache_capture(error);
        CaptureConfig cc = cap->config();
        cc.cardsPerFrame = 1;
        cap->configure(cc, error);
        cap->bind_scene(scene.get());
        cap->bind_radiance(RadianceSampler(&sample_radiance));

        check(cap->update(glm::vec3(0.0f)) == 1, "one card captured");
        CapturedCard first{};
        cap->captured(0, first);
        check(first.card.chunkId == 2, "nearest chunk captured first");
        cap->update(glm::vec3(0.0f));
        cap->captured(1, first);
        check(first.card.chunkId == 1, "far chunk captured second");
    }

    // ---- 8. determinism cross-instance ----
    {
        std::string error;
        auto sceneA = create_lumen_scene(error);
        auto sceneB = create_lumen_scene(error);
        for (auto* s : { sceneA.get(), sceneB.get() }) {
            s->replace_chunk(1, 1, { ground(0.0f, 0.0f), ground(10.0f, 0.0f) }, error);
            s->replace_chunk(2, 1, { ground(0.0f, 20.0f) }, error);
        }
        auto capA = create_surface_cache_capture(error);
        auto capB = create_surface_cache_capture(error);
        capA->bind_scene(sceneA.get());
        capB->bind_scene(sceneB.get());
        capA->bind_radiance(RadianceSampler(&sample_radiance));
        capB->bind_radiance(RadianceSampler(&sample_radiance));
        capA->update(glm::vec3(0.0f));
        capB->update(glm::vec3(0.0f));

        check(capA->captured_count() == capB->captured_count(),
              "twin captured counts");
        bool identical = true;
        for (std::uint32_t i = 0; i < capA->captured_count(); ++i) {
            CapturedCard a{}, b{};
            capA->captured(i, a);
            capB->captured(i, b);
            if (a.card.center != b.card.center || a.irradiance != b.irradiance ||
                a.bouncedRadiance != b.bouncedRadiance ||
                a.captureAge != b.captureAge || a.selfLuminous != b.selfLuminous) {
                identical = false;
                break;
            }
        }
        check(identical, "two captures are bit-identical (determinism)");
    }

    if (g_failures == 0) {
        std::printf("[surface-cache-capture] ALL PASSED\n");
        return 0;
    }
    std::printf("[surface-cache-capture] %d FAILURE(S)\n", g_failures);
    return 1;
}