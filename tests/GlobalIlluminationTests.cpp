// GlobalIlluminationTests.cpp — Agente 1 (task_plan A.19): headless gate for
// the PUBLIC GI/reflection contracts (IGlobalIlluminationProvider / IGiCore /
// IReflectionProvider). Proves the deterministic pure core (toroidal clipmaps,
// per-cascade budget, sun-revision invalidation) and the data-driven backend
// selection + capability-check refusal — no GPU required.
//
// NOTE on the bake lifecycle (faithful to the consolidated RadianceCache,
// task_plan A.2): the initial camera-scroll bake runs at revision 0, and once
// drained the cache commits the CURRENT sun revision with ONE full re-bake
// (rebuild_pending(includeSunStale=true)). A complete cold bake is therefore
// exactly TWO passes over the probes. This is the preserved existing behavior.

#include "engine/rendering/IGlobalIlluminationProvider.hpp"
#include "engine/rendering/IReflectionProvider.hpp"

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

using Engine::Rendering::GiBackend;
using Engine::Rendering::GiCapabilities;
using Engine::Rendering::GiClipmapConfig;
using Engine::Rendering::GiSurfaceSample;
using Engine::Rendering::IGiCore;
using Engine::Rendering::ReflectionBackend;
using Engine::Rendering::ReflectionCapabilities;
using Engine::Rendering::ReflectionSurface;

// A deterministic synthetic terrain: a gentle slope (height = x * 0.1) with a
// constant albedo. Pure function of the inputs -> bit-exact bake.
GiSurfaceSample slope_terrain(float worldX, float worldZ) {
    return GiSurfaceSample{ worldX * 0.1f + worldZ * 0.05f,
                            glm::vec3(0.3f, 0.5f, 0.2f) };
}

// Fully stabilizes the cache: drains the pending bake, then flushes the
// sun-revision commit pass, for BOTH passes. Returns total probes generated.
std::uint32_t stabilize(IGiCore& core, const glm::vec3& camera,
                        const glm::vec3& sun, const glm::vec3& sunColor) {
    std::uint32_t total = 0;
    for (int pass = 0; pass < 2; ++pass) {
        int guard = 0;
        while (core.pending_probe_count() > 0 && guard++ < 100000) {
            total += core.update(camera, sun, sunColor, &slope_terrain, 100000);
        }
        total += core.update(camera, sun, sunColor, &slope_terrain, 100000);
    }
    return total;
}

}  // namespace

int main() {
    using namespace Engine::Rendering;

    // ---- 1. configure (all-or-nothing refusal, never clamps) ----
    {
        std::string error;
        auto core = create_gi_core(error);
        check(core != nullptr, "default gi core created");
        check(error.empty(), "default config diagnostic empty");

        GiClipmapConfig bad = core->config();
        bad.cascadeCount = 0;
        check(!core->configure(bad, error) && !error.empty(),
              "cascadeCount 0 refused all-or-nothing");
        check(core->config().cascadeCount == 6,
              "config unchanged after refusal (never clamps)");
    }

    // ---- 2. JSON round-trip (bit-exact) ----
    {
        std::string error;
        auto a = create_gi_core(error);
        const std::string json = a->config_to_json();
        auto b = create_gi_core_json(json, error);
        check(b != nullptr && error.empty(), "json config loads");
        check(a->config_to_json() == b->config_to_json(),
              "config json round-trip is bit-exact");
    }

    // ---- 3. budget cap + complete cold bake (two passes) ----
    {
        std::string error;
        auto core = create_gi_core(error);
        GiClipmapConfig config = core->config();
        config.cascadeCount = 1;
        config.resolution = 4;         // 4^3 = 64 probes
        config.probesPerFrame = 10;
        config.baseSpacing = 4.0f;
        check(core->configure(config, error) && error.empty(),
              "small config applied");

        const glm::vec3 sun(0.2f, 0.8f, 0.4f);
        const glm::vec3 sunColor(1.0f, 0.9f, 0.7f);

        // First frame: everything is pending; the budget caps the bake at 10.
        const std::uint32_t first =
            core->update(glm::vec3(0.0f), sun, sunColor, &slope_terrain, 0);
        check(first == 10, "first update spends exactly the frame budget");

        // Finish the cold bake: drain + the sun-revision commit pass = 2 passes
        // over the 64 probes.
        const std::uint32_t total =
            first + stabilize(*core, glm::vec3(0.0f), sun, sunColor);
        check(core->pending_probe_count() == 0, "pending drains to zero");
        check(total == 2u * core->total_probe_count(),
              "cold bake = exactly two passes over every probe (initial + sun commit)");

        // A sub-cell camera move must NOT churn the stable cache.
        const std::uint32_t noChurn =
            core->update(glm::vec3(0.5f), sun, sunColor, &slope_terrain, 0);
        check(noChurn == 0, "sub-cell camera move churns zero probes");
    }

    // ---- 4. determinism: two cores reproduce bit-identical radiance ----
    {
        std::string error;
        auto a = create_gi_core(error);
        auto b = create_gi_core(error);
        GiClipmapConfig config = a->config();
        config.cascadeCount = 1;
        config.resolution = 4;
        config.probesPerFrame = 1000;
        check(a->configure(config, error) && b->configure(config, error),
              "twin cores configured");
        const glm::vec3 sun(0.0f, 1.0f, 0.0f);
        const glm::vec3 sunColor(1.0f);
        stabilize(*a, glm::vec3(0.0f), sun, sunColor);
        stabilize(*b, glm::vec3(0.0f), sun, sunColor);
        bool identical = true;
        for (std::uint32_t i = 0; i < a->total_probe_count(); ++i) {
            IGiCore::Probe pa{}, pb{};
            a->probe(i, pa);
            b->probe(i, pb);
            if (pa.radianceVisibility != pb.radianceVisibility ||
                pa.directionConfidence != pb.directionConfidence ||
                pa.worldCellCascade != pb.worldCellCascade) {
                identical = false;
                break;
            }
        }
        check(identical, "two cores bake bit-identical radiance (determinism)");
    }

    // ---- 5. sun refresh re-queues the probes (sun revision) ----
    {
        std::string error;
        auto core = create_gi_core(error);
        GiClipmapConfig config = core->config();
        config.cascadeCount = 1;
        config.resolution = 4;
        config.probesPerFrame = 10;   // small budget so the re-bake is observable
        config.sunRefreshAngleDegrees = 2.0f;
        check(core->configure(config, error), "sun-test config applied");

        const glm::vec3 sunA(0.0f, 1.0f, 0.0f);
        stabilize(*core, glm::vec3(0.0f), sunA, glm::vec3(1.0f));
        check(core->pending_probe_count() == 0, "fully stable before sun change");
        const std::uint32_t rev0 = core->sun_revision();

        // A large sun swing (well beyond the 2-degree threshold) advances the
        // revision and re-queues every probe; the small budget leaves a tail.
        const glm::vec3 sunB(0.7f, 0.7f, 0.0f);
        core->update(glm::vec3(0.0f), sunB, glm::vec3(1.0f), &slope_terrain, 0);
        check(core->sun_revision() > rev0, "sun swing advances the revision");
        check(core->pending_probe_count() > 0,
              "sun swing re-queued probes beyond the frame budget");
    }

    // ---- 6. provider: data-driven selection + capability-check refusal ----
    {
        std::string error;
        GiCapabilities caps;
        caps.ddgi = false;
        caps.rayTraced = false;

        auto provider = create_global_illumination_provider(GiBackend::RadianceCache,
                                                            caps, error);
        check(provider != nullptr && error.empty(),
              "radiance-cache backend always available");
        check(provider->backend() == GiBackend::RadianceCache,
              "backend reported honestly");
        check(provider->core().total_probe_count() > 0,
              "provider core is live");

        auto noDdgi = create_global_illumination_provider(GiBackend::Ddgi, caps,
                                                          error);
        check(noDdgi == nullptr && !error.empty(),
              "DDGI refused when the plugin is not linked (never silent fallback)");

        auto noRt = create_global_illumination_provider(GiBackend::RayTraced, caps,
                                                        error);
        check(noRt == nullptr && !error.empty(),
              "ray-traced refused when the device has no RT");
    }

    // ---- 7. reflection provider: mode decision + capability-check refusal ----
    {
        std::string error;
        ReflectionCapabilities caps;
        caps.probe = false;
        caps.rayTraced = false;
        caps.screenSpace = true;

        auto provider = create_reflection_provider(ReflectionBackend::ScreenSpace,
                                                   caps, error);
        check(provider != nullptr && error.empty(), "screen-space provider created");

        ReflectionSurface rough;
        rough.roughness = 0.8f;
        check(provider->resolve_mode(rough) == ReflectionBackend::None,
              "rough surface gets no reflection without probes");

        ReflectionSurface smooth;
        smooth.roughness = 0.1f;
        check(provider->resolve_mode(smooth) == ReflectionBackend::ScreenSpace,
              "smooth surface gets screen-space reflection");

        ReflectionSurface clearcoat;
        clearcoat.roughness = 0.6f;
        clearcoat.clearCoat = 0.9f;
        check(provider->resolve_mode(clearcoat) == ReflectionBackend::ScreenSpace,
              "clear coat reflects even at higher roughness");

        // RT available: mirror-smooth upgrades to ray-traced.
        ReflectionCapabilities rtCaps;
        rtCaps.probe = false;
        rtCaps.rayTraced = true;
        rtCaps.screenSpace = true;
        auto rtProvider = create_reflection_provider(ReflectionBackend::ScreenSpace,
                                                     rtCaps, error);
        ReflectionSurface mirror;
        mirror.roughness = 0.0f;
        check(rtProvider->resolve_mode(mirror) == ReflectionBackend::RayTraced,
              "mirror upgrades to ray-traced when available");

        auto noProbe = create_reflection_provider(ReflectionBackend::Probe, caps,
                                                  error);
        check(noProbe == nullptr && !error.empty(),
              "probe reflections refused without a radiance cache");
    }

    if (g_failures == 0) {
        std::printf("[global-illumination] ALL PASSED\n");
        return 0;
    }
    std::printf("[global-illumination] %d FAILURE(S)\n", g_failures);
    return 1;
}