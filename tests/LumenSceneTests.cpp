// LumenSceneTests.cpp — Agente 1 (task_plan A.3): headless gate for the
// PUBLIC Lumen-style surface cache (ILumenScene + build_cards). Proves the
// deterministic surface CARD cache with incremental dirty-chunk updates,
// coplanar carding, global/per-chunk budget (LRU), and hierarchical distance
// cascades — no GPU, no voxel types.

#include "engine/rendering/ILumenScene.hpp"

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

using Engine::Rendering::ILumenScene;
using Engine::Rendering::LumenSceneConfig;
using Engine::Rendering::LumenSurface;
using Engine::Rendering::LumenSurfaceCard;
using Engine::Rendering::build_cards;
using Engine::Rendering::create_lumen_scene;

LumenSurface ground(float x, float z, float halfExtent = 1.0f) {
    LumenSurface s;
    s.center = glm::vec3(x, 0.5f, z);
    s.normal = glm::vec3(0.0f, 1.0f, 0.0f);
    s.halfExtent = glm::vec2(halfExtent);
    s.albedo = glm::vec4(0.3f, 0.5f, 0.2f, 1.0f);
    s.emissive = glm::vec3(0.0f);
    return s;
}

}  // namespace

int main() {
    using namespace Engine::Rendering;

    // ---- 1. configure all-or-nothing (never clamps) ----
    {
        std::string error;
        auto scene = create_lumen_scene(error);
        check(scene != nullptr && error.empty(), "default scene created");

        const LumenSceneConfig original = scene->config();

        LumenSceneConfig bad = original;
        bad.maxCards = 0;
        check(!scene->configure(bad, error) && !error.empty(),
              "maxCards 0 refused all-or-nothing");
        check(scene->config().maxCards == original.maxCards,
              "config unchanged after refusal");

        bad = original;
        bad.maxCardsPerChunk = original.maxCards + 1;
        check(!scene->configure(bad, error),
              "maxCardsPerChunk > maxCards refused");

        bad = original;
        bad.cascadeCount = 0;
        check(!scene->configure(bad, error), "cascadeCount 0 refused");

        bad = original;
        bad.cascadeCount = 9;
        check(!scene->configure(bad, error), "cascadeCount 9 refused");

        bad = original;
        bad.cascadeDistance = 0.0f;
        check(!scene->configure(bad, error), "cascadeDistance 0 refused");

        bad = original;
        bad.coplanarDot = 0.4f;
        check(!scene->configure(bad, error), "coplanarDot below range refused");

        bad = original;
        bad.mergeDistance = -1.0f;
        check(!scene->configure(bad, error), "negative mergeDistance refused");
    }

    // ---- 2. incremental dirty-chunk replace + remove (monotonic) ----
    {
        std::string error;
        auto scene = create_lumen_scene(error);
        LumenSceneConfig config = scene->config();
        config.maxCards = 1000;
        config.maxCardsPerChunk = 100;
        check(scene->configure(config, error), "budget config applied");

        std::vector<LumenSurface> one{ ground(0.0f, 0.0f) };
        check(scene->replace_chunk(100, 1, one, error) && error.empty(),
              "first chunk insert");
        check(scene->card_count() == 1, "one card after insert");
        check(scene->chunk_revision(100) == 1, "revision recorded");

        check(!scene->replace_chunk(100, 1, one, error) && !error.empty(),
              "stale revision (== stored) refused");
        check(!scene->replace_chunk(100, 0, one, error),
              "older revision refused");
        check(scene->card_count() == 1, "no change after stale refusal");

        std::vector<LumenSurface> two{ ground(0.0f, 0.0f), ground(100.0f, 100.0f) };
        check(scene->replace_chunk(100, 2, two, error) && error.empty(),
              "revision bump replaces in place");
        check(scene->chunk_card_count(100) == 2, "two cards, no accumulation");
        check(scene->card_count() == 2, "global count replaced, not appended");

        check(scene->remove_chunk(100), "remove_chunk evicts the chunk");
        check(scene->card_count() == 0 && !scene->has_chunk(100),
              "chunk fully evicted");
        check(!scene->remove_chunk(100), "removing unknown chunk is a no-op");
    }

    // ---- 3. coplanar carding (merge) ----
    {
        LumenSceneConfig config;
        config.mergeDistance = 3.0f;
        config.coplanarDot = 0.95f;

        std::vector<LumenSurface> two{ ground(0.0f, 0.0f), ground(2.2f, 0.0f) };
        std::vector<LumenSurfaceCard> merged =
            build_cards(1, 1, two, config);
        check(merged.size() == 1, "two coplanar adjacent surfaces merge into one card");
        check(near(merged[0].center.x, 1.1f),
              "merged center lies between the two patches");
        check(near(merged[0].halfExtent.y, 2.1f),
              "merged tangent extent grows to the union");

        // A perpendicular wall (different normal) must NOT merge with the floor.
        LumenSurface wall = ground(1.0f, 0.0f);
        wall.normal = glm::vec3(1.0f, 0.0f, 0.0f);
        std::vector<LumenSurface> mixed{ ground(0.0f, 0.0f), ground(2.2f, 0.0f), wall };
        std::vector<LumenSurfaceCard> separate = build_cards(2, 1, mixed, config);
        check(separate.size() == 2, "different normal stays a separate card");
    }

    // ---- 4. global budget + LRU eviction ----
    {
        std::string error;
        auto scene = create_lumen_scene(error);
        LumenSceneConfig config = scene->config();
        config.maxCards = 4;
        config.maxCardsPerChunk = 4;
        check(scene->configure(config, error), "tight budget applied");

        std::vector<LumenSurface> pair{ ground(0.0f, 0.0f), ground(100.0f, 0.0f) };
        check(scene->replace_chunk(1, 1, pair, error), "chunk 1 in");
        check(scene->replace_chunk(2, 1, pair, error), "chunk 2 in");
        check(scene->card_count() == 4, "budget full at 4 cards");

        check(scene->replace_chunk(3, 1, pair, error), "chunk 3 in");
        check(scene->card_count() == 4, "insert over budget stays at budget");
        check(!scene->has_chunk(1), "least-recently-used chunk evicted");
        check(scene->has_chunk(2) && scene->has_chunk(3),
              "recent chunks survive eviction");
    }

    // ---- 5. hierarchical distance cascades (distant representation) ----
    {
        std::string error;
        auto scene = create_lumen_scene(error);
        LumenSceneConfig config = scene->config();
        config.cascadeCount = 3;
        config.cascadeDistance = 10.0f;
        check(scene->configure(config, error), "cascade config applied");

        check(scene->replace_chunk(1, 1, { ground(0.0f, 0.0f) }, error), "near");
        check(scene->replace_chunk(2, 1, { ground(15.0f, 0.0f) }, error), "mid");
        check(scene->replace_chunk(3, 1, { ground(50.0f, 0.0f) }, error), "far");

        scene->update(glm::vec3(0.0f));
        check(scene->cards_in_cascade(0) == 1, "near card in cascade 0");
        check(scene->cards_in_cascade(1) == 1, "mid card in cascade 1");
        check(scene->cards_in_cascade(2) == 1, "far card in cascade 2");
    }

    // ---- 6. determinism: two scenes reproduce bit-identical state ----
    {
        std::string error;
        auto a = create_lumen_scene(error);
        auto b = create_lumen_scene(error);
        LumenSceneConfig config = a->config();
        config.maxCards = 1000;
        config.maxCardsPerChunk = 100;
        config.cascadeCount = 3;
        config.cascadeDistance = 10.0f;
        check(a->configure(config, error) && b->configure(config, error),
              "twin scenes configured");

        std::vector<LumenSurface> floor{ ground(0.0f, 0.0f), ground(2.2f, 0.0f) };
        std::vector<LumenSurface> wall{ ground(1.0f, 0.0f) };
        wall[0].normal = glm::vec3(1.0f, 0.0f, 0.0f);
        a->replace_chunk(1, 1, floor, error);
        a->replace_chunk(2, 1, wall, error);
        b->replace_chunk(1, 1, floor, error);
        b->replace_chunk(2, 1, wall, error);
        a->update(glm::vec3(0.0f));
        b->update(glm::vec3(0.0f));

        check(a->card_count() == b->card_count(), "twin card counts");
        bool identical = true;
        for (std::uint32_t i = 0; i < a->card_count(); ++i) {
            LumenSurfaceCard ca{}, cb{};
            a->card(i, ca);
            b->card(i, cb);
            if (ca.center != cb.center || ca.normal != cb.normal ||
                ca.halfExtent != cb.halfExtent || ca.albedo != cb.albedo ||
                ca.emissive != cb.emissive || ca.chunkId != cb.chunkId ||
                ca.revision != cb.revision || ca.cascade != cb.cascade) {
                identical = false;
                break;
            }
        }
        check(identical, "two scenes are bit-identical (determinism)");
    }

    // ---- 7. all-or-nothing validation leaves the cache untouched ----
    {
        std::string error;
        auto scene = create_lumen_scene(error);
        std::vector<LumenSurface> good{ ground(0.0f, 0.0f) };
        check(scene->replace_chunk(1, 1, good, error), "valid chunk in");

        std::vector<LumenSurface> badCenter{ good[0] };
        badCenter[0].center.x = std::nanf("");
        check(!scene->replace_chunk(1, 2, badCenter, error) && !error.empty(),
              "NaN center refused");
        check(scene->chunk_revision(1) == 1 && scene->card_count() == 1,
              "cache untouched after NaN refusal");

        std::vector<LumenSurface> badNormal{ good[0] };
        badNormal[0].normal = glm::vec3(0.0f);
        check(!scene->replace_chunk(1, 2, badNormal, error),
              "zero normal refused");
        check(scene->chunk_revision(1) == 1 && scene->card_count() == 1,
              "cache untouched after zero-normal refusal");
    }

    if (g_failures == 0) {
        std::printf("[lumen-scene] ALL PASSED\n");
        return 0;
    }
    std::printf("[lumen-scene] %d FAILURE(S)\n", g_failures);
    return 1;
}