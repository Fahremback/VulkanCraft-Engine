// ToneMappingTests.cpp — Agente 1 (task_plan B.7): headless gate for the
// PUBLIC HDR tone mapping contract (IToneMapping). Proves the deterministic
// pure core: EV/manual exposure, ACES / Reinhard / Filmic operators, HDR
// emissive saturation and exact linear <-> sRGB color management — no GPU
// required.

#include "engine/rendering/IToneMapping.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

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

bool near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

using Engine::Rendering::IToneMapping;
using Engine::Rendering::ToneMappingConfig;
using Engine::Rendering::ToneOperator;
using Engine::Rendering::create_tone_mapping;
using Engine::Rendering::create_tone_mapping_json;

}  // namespace

int main() {
    using namespace Engine::Rendering;

    // ---- 1. default config + all-or-nothing refusal ----
    {
        std::string error;
        auto t = create_tone_mapping(error);
        check(t != nullptr, "default tone mapping created");
        check(error.empty(), "default config diagnostic empty");

        ToneMappingConfig bad = t->config();
        bad.exposure = 0.001f;
        check(!t->configure(bad, error) && !error.empty(), "exposure 0.001 refused");
        bad = t->config();
        bad.ev100 = 20.0f;
        check(!t->configure(bad, error) && !error.empty(), "ev100 20 refused");
        bad = t->config();
        bad.whitePoint = 0.5f;
        check(!t->configure(bad, error) && !error.empty(), "whitePoint 0.5 refused");
        bad = t->config();
        bad.op = static_cast<ToneOperator>(99);
        check(!t->configure(bad, error) && !error.empty(), "op out of range refused");
        bad = t->config();
        bad.exposure = std::numeric_limits<float>::quiet_NaN();
        check(!t->configure(bad, error) && !error.empty(), "NaN refused");
        check(t->config().exposure == 1.0f,
              "config unchanged after refusals (never clamps)");
    }

    // ---- 2. JSON round-trip (bit-exact) + refusals ----
    {
        std::string error;
        auto a = create_tone_mapping(error);
        ToneMappingConfig c = a->config();
        c.op = ToneOperator::Filmic;
        c.exposure = 0.7f;
        c.useEV = true;
        c.ev100 = -2.0f;
        c.whitePoint = 8.0f;
        check(a->configure(c, error), "custom config applied");

        auto b = create_tone_mapping_json(a->config_to_json(), error);
        check(b != nullptr && error.empty(), "json round-trip creates");
        check(b->config().op == ToneOperator::Filmic &&
                  b->config().exposure == 0.7f && b->config().useEV &&
                  b->config().ev100 == -2.0f && b->config().whitePoint == 8.0f,
              "json round-trip bit-exact");

        check(create_tone_mapping_json("{ \"version\": 1, \"op\": \"bogus\" }",
                                       error) == nullptr,
              "unknown op refused");
        check(create_tone_mapping_json("{ \"version\": 1, \"bogus\": 1 }",
                                       error) == nullptr,
              "unknown key refused");
        check(create_tone_mapping_json("{ \"version\": 2 }", error) == nullptr,
              "unsupported version refused");
        check(create_tone_mapping_json(
                  "{ \"version\": 1, \"useEV\": \"maybe\" }", error) == nullptr,
              "invalid bool refused");
        check(create_tone_mapping_json(
                  "{ \"version\": 1, \"exposure\": 100.0 }", error) == nullptr,
              "invalid json config refused all-or-nothing");
    }

    // ---- 3. exposure: manual and EV ----
    {
        std::string error;
        auto t = create_tone_mapping(error);
        check(near(t->exposureFactor(), 1.0f), "default exposure = 1");

        ToneMappingConfig c = t->config();
        c.exposure = 2.0f;
        check(t->configure(c, error), "manual 2.0 applied");
        check(near(t->exposureFactor(), 2.0f), "manual exposure factor");

        c.useEV = true;
        c.ev100 = 0.0f;
        check(t->configure(c, error), "EV 0 applied");
        check(near(t->exposureFactor(), 1.0f / 1.2f),
              "EV 0 -> 1/(1.2*2^0) = 0.833");
        c.ev100 = 1.0f;
        check(t->configure(c, error), "EV 1 applied");
        check(near(t->exposureFactor(), 1.0f / 2.4f),
              "EV 1 -> 1/(1.2*2^1) = 0.417");
        c.ev100 = -2.0f;
        check(t->configure(c, error), "EV -2 applied");
        check(near(t->exposureFactor(), 1.0f / 0.3f),
              "EV -2 -> 1/(1.2*0.25) = 3.333 (brighter)");
        check(t->exposureFactor() > 1.0f,
              "negative EV brightens (factor > 1)");
    }

    // ---- 4. operators: Reinhard / ACES / Filmic / None ----
    {
        std::string error;
        auto t = create_tone_mapping(error);

        ToneMappingConfig c = t->config();
        c.op = ToneOperator::Reinhard;
        c.exposure = 1.0f;
        c.useEV = false;
        check(t->configure(c, error), "reinhard applied");
        check(near(t->tonemapChannel(0.0f), 0.0f), "reinhard 0 -> 0");
        check(near(t->tonemapChannel(1.0f), 0.5f), "reinhard 1 -> 0.5");
        check(near(t->tonemapChannel(4.0f), 0.8f), "reinhard 4 -> 0.8");
        check(t->tonemapChannel(0.5f) < t->tonemapChannel(2.0f),
              "reinhard monotonic");

        c.op = ToneOperator::ACES;
        check(t->configure(c, error), "aces applied");
        check(near(t->tonemapChannel(0.0f), 0.0f), "aces 0 -> 0");
        check(near(t->tonemapChannel(0.5f), 0.6163f), "aces 0.5 -> 0.6163");
        check(near(t->tonemapChannel(1.0f), 2.54f / 3.16f),
              "aces 1 -> 0.8038 (saturate form)");
        check(t->tonemapChannel(1000.0f) > 0.999f,
              "aces HDR: emissive 1000 saturates near 1 (asymptote 1.033)");
        check(t->tonemapChannel(0.5f) < t->tonemapChannel(1.0f),
              "aces monotonic");

        c.op = ToneOperator::Filmic;
        check(t->configure(c, error), "filmic applied");
        check(near(t->tonemapChannel(0.0f), 0.0f), "filmic 0 -> 0");
        check(near(t->tonemapChannel(11.2f), 1.0f),
              "filmic white point 11.2 -> 1 (normalized)");
        check(near(t->tonemapChannel(0.18f), 0.0671f),
              "filmic 0.18 -> 0.067 (soft mid rolloff, not a hard clamp)");
        check(near(t->tonemapChannel(0.5f), 0.1720f),
              "filmic 0.5 -> 0.172 (compresses mid-tones smoothly)");
        check(t->tonemapChannel(11.2f) >= t->tonemapChannel(5.0f),
              "filmic monotonic to the white point");

        c.op = ToneOperator::None;
        check(t->configure(c, error), "none applied");
        check(near(t->tonemapChannel(2.0f), 1.0f), "none: clamps to 1");
        check(near(t->tonemapChannel(0.5f), 0.5f), "none: passes through");
    }

    // ---- 5. apply: full pipeline + HDR emissive consistency ----
    {
        std::string error;
        auto t = create_tone_mapping(error);

        // ACES with exposure 1: emissive 1000 saturates consistently
        const glm::vec3 hot(1000.0f, 1000.0f, 1000.0f);
        const glm::vec3 a = t->apply(hot);
        check(a.x > 0.99f && a.x <= 1.0f,
              "HDR emissive 1000 saturates near 1 (consistent)");
        check(a.y == a.x && a.z == a.x, "grey emissive stays grey");

        // dimmer emissive maps lower: consistency, not just clamping
        const glm::vec3 b = t->apply(glm::vec3(2.0f, 2.0f, 2.0f));
        check(b.x < a.x, "dimmer emissive maps lower (curve, not hard clamp)");

        // exposure scales before the operator
        ToneMappingConfig c = t->config();
        c.exposure = 0.5f;
        check(t->configure(c, error), "exposure 0.5 applied");
        const glm::vec3 d = t->apply(glm::vec3(1.0f, 1.0f, 1.0f));
        check(near(d.x, t->tonemapChannel(0.5f)),
              "apply = tonemap(exposure * linear)");
    }

    // ---- 6. color management: exact linear <-> sRGB ----
    {
        check(near(IToneMapping::linearToSrgb(0.0f), 0.0f, 1e-6f),
              "linear 0 -> srgb 0");
        check(near(IToneMapping::linearToSrgb(1.0f), 1.0f, 1e-6f),
              "linear 1 -> srgb 1");
        check(near(IToneMapping::linearToSrgb(0.0031308f), 12.92f * 0.0031308f),
              "linear low end is the 12.92 slope (piecewise exact)");
        check(near(IToneMapping::linearToSrgb(0.5f), 1.055f * std::pow(0.5f, 1.0f / 2.4f) - 0.055f),
              "linear 0.5 -> srgb 0.7354 (piecewise exact)");

        check(near(IToneMapping::srgbToLinear(0.0f), 0.0f, 1e-6f),
              "srgb 0 -> linear 0");
        check(near(IToneMapping::srgbToLinear(1.0f), 1.0f, 1e-6f),
              "srgb 1 -> linear 1");
        check(near(IToneMapping::srgbToLinear(0.5f),
                   std::pow((0.5f + 0.055f) / 1.055f, 2.4f)),
              "srgb 0.5 -> linear 0.2140 (piecewise exact)");

        // round-trip identity (within float error)
        bool rt = true;
        for (float v = 0.0f; v <= 1.0f; v += 0.05f) {
            const float back = IToneMapping::linearToSrgb(
                IToneMapping::srgbToLinear(v));
            if (std::fabs(back - v) > 2e-3f) rt = false;
        }
        check(rt, "srgb -> linear -> srgb round-trips");

        // vec3 forms
        const glm::vec3 c(0.1f, 0.5f, 0.9f);
        const glm::vec3 out = IToneMapping::linearToSrgb(c);
        check(near(out.x, IToneMapping::linearToSrgb(c.x)) &&
                  near(out.y, IToneMapping::linearToSrgb(c.y)) &&
                  near(out.z, IToneMapping::linearToSrgb(c.z)),
              "vec3 linearToSrgb applies per channel");
        const glm::vec3 back = IToneMapping::srgbToLinear(out);
        check(near(back.x, c.x, 2e-3f) && near(back.y, c.y, 2e-3f) &&
                  near(back.z, c.z, 2e-3f),
              "vec3 round-trip");
    }

    // ---- 7. determinism (bit-exact) ----
    {
        std::string error;
        auto t = create_tone_mapping(error);
        const glm::vec3 v(0.3f, 1.7f, 9.0f);
        const glm::vec3 a = t->apply(v);
        const glm::vec3 b = t->apply(v);
        check(std::memcmp(&a, &b, sizeof(glm::vec3)) == 0,
              "apply reproduces bit-exact results");
    }

    if (g_failures == 0) {
        std::printf("[tone-mapping] ALL PASSED\n");
        return 0;
    }
    std::printf("[tone-mapping] %d FAILURE(S)\n", g_failures);
    return 1;
}
