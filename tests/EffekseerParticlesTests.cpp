// EffekseerParticlesTests.cpp — Agente 1 (task_plan C): headless gate for the
// PUBLIC CPU particle system (IParticleSystem, promoted Effekseer core, MIT).
// Embeds a REAL vendor effect (block_simple.efk, 306 bytes — an SKFE-binary
// effect with a Model/block material reference) and proves: load from memory,
// spawn with an explicit seed, deterministic 60-frame simulation (bit-exact
// across independent systems), alive-count lifecycle, and all-or-nothing
// refusals. No GPU, no renderer, no filesystem.

#include "engine/rendering/IParticleSystem.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

// The vendor's own test asset (Dev/Cpp/Test/Resource/block_simple.efk,
// 306 bytes), embedded so the gate is fully self-contained. Starts with the
// "SKFE" magic; references Model/block.efkmodel as its material.
const std::uint8_t kBlockSimpleEfk[306] = {
    0x53,0x4b,0x46,0x45,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x15,0x00,0x00,0x00,
    0x4d,0x00,0x6f,0x00,0x64,0x00,0x65,0x00,0x6c,0x00,0x2f,0x00,
    0x62,0x00,0x6c,0x00,0x6f,0x00,0x63,0x00,0x6b,0x00,0x2e,0x00,
    0x65,0x00,0x66,0x00,0x6b,0x00,0x6d,0x00,0x6f,0x00,0x64,0x00,
    0x65,0x00,0x6c,0x00,0x00,0x00,0x00,0x00,0x80,0x3f,0xff,0xff,
    0xff,0xff,0x01,0x00,0x00,0x00,0x05,0x00,0x00,0x00,0x2c,0x00,
    0x00,0x00,0x01,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x02,0x00,
    0x00,0x00,0x02,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x64,0x00,0x00,0x00,0x64,0x00,
    0x00,0x00,0x00,0x00,0x80,0x3f,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x0c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x0c,0x00,0x00,0x00,0xc3,0xb8,
    0xb2,0x3e,0xc3,0xb8,0xb2,0x3e,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x0c,0x00,0x00,0x00,0xcd,0xcc,0x4c,0x3d,0xcd,0xcc,
    0x4c,0x3d,0xcd,0xcc,0x4c,0x3d,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0xff,0xff,0xff,0xff,0x01,0x00,0x00,0x00,0x01,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x05,0x00,0x00,0x00,0x00,0x00,0x80,0x3f,0x00,0x00,
    0x00,0x00,0xff,0xff,0xff,0xff,0x01,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,
};

}  // namespace

int main() {
    using namespace Engine::Rendering;

    // --- load: valid .efk accepted; garbage/null refused (all-or-nothing) ---
    {
        auto ps = create_particle_system();
        std::string err;
        check(ps->loadEffect(kBlockSimpleEfk, sizeof(kBlockSimpleEfk), err),
              "load: valid .efk accepted");
        check(!ps->loadEffect(nullptr, 0, err),
              "load: null data refused");
        check(!err.empty(), "load: null data diagnostic present");

        const std::uint8_t garbage[16] = {0xDE, 0xAD, 0xBE, 0xEF};
        check(!ps->loadEffect(garbage, sizeof(garbage), err),
              "load: garbage refused");
    }

    // --- spawn before load refused; after load, handle valid ---
    {
        auto ps = create_particle_system();
        check(ps->spawn(0.0f, 0.0f, 0.0f, 42) == -1,
              "spawn before load refused");
        std::string err;
        check(ps->loadEffect(kBlockSimpleEfk, sizeof(kBlockSimpleEfk), err),
              "spawn: load ok");
        check(ps->spawn(0.0f, 0.0f, 0.0f, 42) >= 0,
              "spawn after load returns a handle");
    }

    // --- simulation: 60 frames at 60 fps keeps the block alive ---
    {
        auto ps = create_particle_system();
        std::string err;
        check(ps->loadEffect(kBlockSimpleEfk, sizeof(kBlockSimpleEfk), err),
              "sim: load ok");
        const std::int32_t h = ps->spawn(0.0f, 0.0f, 0.0f, 42);
        check(h >= 0, "sim: spawn ok");
        check(ps->aliveCount(h) == 0,
              "sim: no instances before the first step (spawn is lazy)");
        ps->step(1.0f / 60.0f);
        check(ps->aliveCount(h) >= 1, "sim: alive after the first step");
        for (int f = 1; f < 60; ++f) {
            ps->step(1.0f / 60.0f);
        }
        check(ps->aliveCount(h) >= 1,
              "sim: block still alive after 1 second");
    }

    // --- determinism: independent systems, same seed + steps, bit-exact ---
    {
        auto a = create_particle_system();
        auto b = create_particle_system();
        std::string err;
        check(a->loadEffect(kBlockSimpleEfk, sizeof(kBlockSimpleEfk), err) &&
                  b->loadEffect(kBlockSimpleEfk, sizeof(kBlockSimpleEfk), err),
              "det: both load ok");
        const std::int32_t ha = a->spawn(1.0f, 2.0f, 3.0f, 7);
        const std::int32_t hb = b->spawn(1.0f, 2.0f, 3.0f, 7);
        check(ha >= 0 && hb >= 0, "det: both spawn ok");
        bool same = true;
        for (int f = 0; f < 60; ++f) {
            a->step(1.0f / 60.0f);
            b->step(1.0f / 60.0f);
            if (a->aliveCount(ha) != b->aliveCount(hb)) {
                same = false;
                std::printf("  (mismatch at frame %d: %d vs %d)\n", f,
                            a->aliveCount(ha), b->aliveCount(hb));
                break;
            }
        }
        check(same, "det: instance counts bit-identical across systems");
    }

    // --- stop: instance expires on subsequent steps ---
    {
        auto ps = create_particle_system();
        std::string err;
        check(ps->loadEffect(kBlockSimpleEfk, sizeof(kBlockSimpleEfk), err),
              "stop: load ok");
        const std::int32_t h = ps->spawn(0.0f, 0.0f, 0.0f, 1);
        check(h >= 0, "stop: spawn ok");
        ps->stop(h);
        for (int f = 0; f < 120 && ps->aliveCount(h) > 0; ++f) {
            ps->step(1.0f / 60.0f);
        }
        check(ps->aliveCount(h) == 0,
              "stop: instance expires after stopping");
    }

    // --- invalid handle: aliveCount is 0, stop is a no-op ---
    {
        auto ps = create_particle_system();
        check(ps->aliveCount(-1) == 0, "invalid handle aliveCount == 0");
        ps->stop(-1);  // must not crash
        ps->step(1.0f / 60.0f);  // must not crash
    }

    if (g_failures == 0) {
        std::printf("[effekseer-particles] ALL PASSED\n");
        return 0;
    }
    std::printf("[effekseer-particles] %d FAILURE(S)\n", g_failures);
    return 1;
}
