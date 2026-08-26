// DiffuseGlobalIlluminationTests.cpp — Agente 1 (task_plan A.5): headless gate
// for the PUBLIC multi-bounce diffuse GI (IDiffuseGlobalIllumination). Proves
// emissive sources, color bleeding across bounces, shadowed skylight, energy
// tracking and determinism — no GPU.

#include "engine/rendering/IDiffuseGlobalIllumination.hpp"

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

bool near(float a, float b, float eps = 1.0e-2f) {
    return std::fabs(a - b) <= eps;
}

using Engine::Rendering::CapturedCard;
using Engine::Rendering::DiffuseGiConfig;
using Engine::Rendering::DiffuseGiResult;
using Engine::Rendering::IDiffuseGlobalIllumination;
using Engine::Rendering::create_diffuse_global_illumination;

CapturedCard wall(float x, glm::vec3 normal, glm::vec4 albedo,
                  glm::vec3 emissive = glm::vec3(0.0f),
                  glm::vec4 irradiance = glm::vec4(0.0f)) {
    CapturedCard c;
    c.card.center = glm::vec3(x, 0.5f, 0.0f);
    c.card.normal = normal;
    c.card.halfExtent = glm::vec2(0.5f, 0.5f);
    c.card.albedo = albedo;
    c.card.emissive = emissive;
    c.card.chunkId = 1;
    c.card.revision = 1;
    c.irradiance = irradiance;
    c.selfLuminous = glm::dot(emissive, emissive) > 0.0f;
    return c;
}

}  // namespace

int main() {
    using namespace Engine::Rendering;

    // ---- 1. configure all-or-nothing ----
    {
        std::string error;
        auto gi = create_diffuse_global_illumination(error);
        check(gi != nullptr && error.empty(), "gi created");

        DiffuseGiConfig bad = gi->config();
        bad.bounces = 0;
        check(!gi->configure(bad, error) && !error.empty(), "bounces 0 refused");
        bad = gi->config();
        bad.bounces = 9;
        check(!gi->configure(bad, error), "bounces 9 refused");
        bad = gi->config();
        bad.maxDistance = 0.0f;
        check(!gi->configure(bad, error), "maxDistance 0 refused");
        bad = gi->config();
        bad.intensity = 0.0f;
        check(!gi->configure(bad, error), "intensity 0 refused");
        bad = gi->config();
        bad.skylight = glm::vec3(std::nanf(""), 0.0f, 0.0f);
        check(!gi->configure(bad, error), "NaN skylight refused");
    }

    // ---- 2. emissive source lights a receiver ----
    {
        std::string error;
        auto gi = create_diffuse_global_illumination(error);
        DiffuseGiConfig config = gi->config();
        config.bounces = 1;
        config.skylight = glm::vec3(0.0f);
        config.maxDistance = 128.0f;
        gi->configure(config, error);

        std::vector<CapturedCard> cards{
            wall(0.0f, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec4(1.0f),
                 glm::vec3(10.0f, 0.0f, 0.0f)),   // red emissive
            wall(2.0f, glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec4(1.0f)),  // white receiver
        };
        check(gi->set_cards(cards, error) && error.empty(), "cards bound");
        check(gi->solve(error) && error.empty(), "solve ok");

        DiffuseGiResult receiver{};
        check(gi->result(1, receiver), "receiver result");
        check(receiver.indirect.r > 0.5f, "receiver gathers the red emitter");
        check(near(receiver.indirect.g, 0.0f) && near(receiver.indirect.b, 0.0f),
              "pure red emitter bleeds no green/blue");
        check(receiver.outgoing.r > 0.5f, "outgoing reflects the gathered light");
    }

    // ---- 3. color bleeding across bounces ----
    {
        std::string error;
        auto gi = create_diffuse_global_illumination(error);
        DiffuseGiConfig config = gi->config();
        config.skylight = glm::vec3(0.0f);
        config.maxDistance = 3.0f;  // A->C (dist 4) is culled; A->B->C survives

        std::vector<CapturedCard> cards{
            wall(0.0f, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec4(1.0f),
                 glm::vec3(10.0f, 10.0f, 10.0f)),                    // white emitter
            wall(2.0f, glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)), // red surface
            wall(4.0f, glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec4(1.0f)), // white, sees only red
        };
        check(gi->set_cards(cards, error), "3-card chain bound");

        config.bounces = 1;
        gi->configure(config, error);
        gi->solve(error);
        DiffuseGiResult c1{};
        gi->result(2, c1);
        check(near(c1.indirect.r, 0.0f) && near(c1.indirect.g, 0.0f) &&
                  near(c1.indirect.b, 0.0f),
              "one bounce: the far card is still dark (A culled)");

        config.bounces = 2;
        gi->configure(config, error);
        gi->solve(error);
        DiffuseGiResult c2{};
        gi->result(2, c2);
        check(c2.indirect.r > 0.02f, "two bounces: far card gathers light");
        check(c2.indirect.r > c2.indirect.g && near(c2.indirect.g, c2.indirect.b, 1.0e-3f),
              "color bleeding: the red surface tints the far card red");
    }

    // ---- 4. shadowed skylight ----
    {
        std::string error;
        auto gi = create_diffuse_global_illumination(error);
        DiffuseGiConfig config = gi->config();
        config.bounces = 1;
        config.skylight = glm::vec3(1.0f, 1.0f, 1.0f);
        gi->configure(config, error);

        std::vector<CapturedCard> cards{
            wall(0.0f, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec4(1.0f),
                 glm::vec3(0.0f), glm::vec4(0.0f, 0.0f, 0.0f, 0.0f)),  // skyVis 0
            wall(2.0f, glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec4(1.0f),
                 glm::vec3(0.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)),  // skyVis 1
        };
        gi->set_cards(cards, error);
        gi->solve(error);

        DiffuseGiResult occluded{}, open{};
        gi->result(0, occluded);
        gi->result(1, open);
        check(near(occluded.direct.r, 0.0f) && near(occluded.direct.g, 0.0f) &&
                  near(occluded.direct.b, 0.0f),
              "skyVis 0 -> no skylight (shadowed)");
        check(near(open.direct.r, 1.0f) && near(open.direct.g, 1.0f) &&
                  near(open.direct.b, 1.0f),
              "skyVis 1 -> full skylight");
    }

    // ---- 5. energy tracking + determinism ----
    {
        std::string error;
        auto a = create_diffuse_global_illumination(error);
        auto b = create_diffuse_global_illumination(error);
        DiffuseGiConfig config = a->config();
        config.bounces = 2;
        config.skylight = glm::vec3(0.0f);
        config.maxDistance = 128.0f;
        a->configure(config, error);
        b->configure(config, error);

        std::vector<CapturedCard> cards{
            wall(0.0f, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec4(1.0f),
                 glm::vec3(5.0f, 0.0f, 0.0f)),
            wall(2.0f, glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec4(0.5f)),
        };
        a->set_cards(cards, error);
        b->set_cards(cards, error);
        a->solve(error);
        b->solve(error);

        check(a->bounce_energy(1) > 0.0f, "first bounce injects energy");
        check(a->bounce_energy(2) > 0.0f, "second bounce is finite and positive");

        bool identical = true;
        for (std::uint32_t i = 0; i < a->card_count(); ++i) {
            DiffuseGiResult ra{}, rb{};
            a->result(i, ra);
            b->result(i, rb);
            if (ra.direct != rb.direct || ra.indirect != rb.indirect ||
                ra.outgoing != rb.outgoing) {
                identical = false;
                break;
            }
        }
        check(identical, "two solves are bit-identical (determinism)");
    }

    if (g_failures == 0) {
        std::printf("[diffuse-global-illumination] ALL PASSED\n");
        return 0;
    }
    std::printf("[diffuse-global-illumination] %d FAILURE(S)\n", g_failures);
    return 1;
}