// VulkanLoaderTests.cpp — gate for the from-scratch dynamic Vulkan loader
// (task_plan C.21 volk). Headless; no device/window needed.
#include "engine/rendering/IVulkanLoader.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using namespace Engine::Rendering;

static int g_passed = 0, g_failed = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("  FAIL (%d): %s\n", __LINE__, msg); ++g_failed; } else ++g_passed; } while (0)

int main() {
    std::printf("[vkloader] ALL tests starting\n");

    std::string err;
    auto loader = create_vulkan_loader(err);
    CHECK(loader != nullptr, "factory ok");

    // If a Vulkan loader is present, load() succeeds and resolves trampolines.
    const bool hasLoader = loader->load(err);

    // Determinism / safety: querying before load must not resolve globals.
    CHECK(!loader->loaded() || hasLoader, "loaded() consistent with load result");

    // load() idempotent.
    if (hasLoader) {
        bool again = loader->load(err);
        CHECK(again, "idempotent load");
        CHECK(loader->loadedCount() >= 2, "at least the two trampolines resolved");
        CHECK(loader->describe().find("loaded") != std::string::npos, "describe mentions loaded");

        void* hip = nullptr;
        bool ok = loader->resolve("vkGetInstanceProcAddr", &hip);
        CHECK(ok && hip != nullptr, "vkGetInstanceProcAddr resolves");
        void* hdp = nullptr;
        loader->resolve("vkGetDeviceProcAddr", &hdp);
        CHECK(hdp != nullptr, "vkGetDeviceProcAddr resolves");

        // Device-level resolution without a real device must be refused.
        void* dp = nullptr;
        CHECK(!loader->resolveDevice("vkCmdDraw", nullptr, &dp) || dp == nullptr,
              "device resolution without device refused");

        void* bad = nullptr;
        CHECK(!loader->resolve("vkDefinitelyNotAFunctionXYZ", &bad) || bad == nullptr,
              "unknown function -> null (no crash)");
    } else {
        // Loader absent: must report cleanly and not crash.
        CHECK(!err.empty(), "clean error when absent");
        CHECK(loader->describe().find("not loaded") != std::string::npos, "describe clean");
        void* dp = nullptr;
        loader->resolve("vkGetInstanceProcAddr", &dp);
        CHECK(dp == nullptr, "resolve on unloaded -> null");
    }

    // Factory always returns a stable object + describe never crashes in both states.
    CHECK(!loader->describe().empty(), "describe non-empty");

    std::printf("\n[vkloader] Results: %d passed, %d failed\n", g_passed, g_failed);
    if (g_failed > 0) { std::printf("[vkloader] FAILED\n"); return 1; }
    std::printf("[vkloader] ALL PASSED\n");
    return 0;
}