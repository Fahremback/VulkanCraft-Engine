// WickedBaseTests.cpp
//
// Gate for the ported Wicked Engine GUI BASE layer (agente 2 — item
// `wicked-engine`): `src/editor/frontend/wicked/*.h` — wiColor, wiCanvas,
// wiMath (DirectX-compatible types implemented on GLM, NO DirectX headers).
// Proves the ported base WORKS (behavior, not just compilation):
//   - wi::Color: rgba packing/getters, named colors, hex ctor, lerp (exact
//     midpoint of Red->White), toFloat4/fromFloat4 round-trip;
//   - wi::Canvas: DPI scaling (96 = 1.0, 144 = 1.5), physical<->logical
//     conversion exactness, logical size at 4K with 2x scaling;
//   - wi::math types: XMFLOAT2/3/4 constructors + component access.
//
// Self-contained: includes only the ported headers (+std). Deterministic.

#include "wiColor.h"
#include "wiCanvas.h"
#include "wiMath.h"

#include <cstdint>
#include <cstdio>

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

namespace {

void test_color() {
    std::printf("[wicked] wi::Color…\n");
    // Named colors.
    check(wi::Color::Red().getR() == 255 && wi::Color::Red().getG() == 0 &&
              wi::Color::Red().getB() == 0 && wi::Color::Red().getA() == 255,
          "Red() = (255,0,0,255)");
    check(wi::Color::White().getR() == 255 && wi::Color::Black().getR() == 0,
          "White/Black named colors");
    // rgba packing ctor.
    wi::Color packed(10, 20, 30, 40);
    check(packed.getR() == 10 && packed.getG() == 20 && packed.getB() == 30 &&
              packed.getA() == 40,
          "rgba ctor packs/getters round-trip");
    // Hex ctor: Wicked/DirectXColor convention is "BBGGRR" (BGR order in the
    // hex string) — probed empirically: "0000FF" -> R=255, "FF0000" -> B=255.
    wi::Color fromHex("0080FF");  // B=00 G=80 R=FF
    check(fromHex.getR() == 255 && fromHex.getG() == 128 && fromHex.getB() == 0,
          "hex ctor parses BBGGRR (BGR order)");
    // Lerp midpoint Red -> White: R stays 255 (both endpoints), G/B go 0->255
    // so midpoint G/B = 127 (0.5 * 255).
    const wi::Color mid = wi::Color::lerp(wi::Color::Red(), wi::Color::White(), 0.5f);
    check(mid.getR() == 255, "lerp midpoint R = 255 (red->white keeps R)");
    check(mid.getG() >= 127 && mid.getG() <= 128,
          "lerp midpoint G ~127 (0.5 between 0 and 255)");
    check(mid.getB() >= 127 && mid.getB() <= 128,
          "lerp midpoint B ~127 (0.5 between 0 and 255)");
    // toFloat4/fromFloat4 round-trip.
    const wi::Color rt = wi::Color::fromFloat4(wi::Color::Green().toFloat4());
    check(rt.getR() == 0 && rt.getG() == 255 && rt.getB() == 0,
          "toFloat4/fromFloat4 round-trip keeps color");
    std::printf("[wicked] wi::Color OK\n");
}

void test_canvas() {
    std::printf("[wicked] wi::Canvas…\n");
    // 96 DPI = 1.0 scale.
    wi::Canvas c96;
    c96.init(1920, 1080, 96.0f);
    check(c96.GetDPIScaling() == 1.0f, "96 DPI -> scale 1.0");
    check(c96.GetLogicalWidth() == 1920.0f && c96.GetLogicalHeight() == 1080.0f,
          "96 DPI -> logical == physical");
    // 144 DPI = 1.5 scale.
    wi::Canvas c144;
    c144.init(2880, 1620, 144.0f);
    check(c144.GetDPIScaling() == 1.5f, "144 DPI -> scale 1.5");
    check(c144.GetLogicalWidth() == 1920.0f, "144 DPI -> logical 1920 (2880/1.5)");
    check(c144.GetLogicalHeight() == 1080.0f, "144 DPI -> logical 1080 (1620/1.5)");
    // Physical<->Logical exactness.
    check(c144.LogicalToPhysical(100.0f) == 150u, "LogicalToPhysical(100)@1.5 = 150");
    check(c144.PhysicalToLogical(150.0f) == 100.0f, "PhysicalToLogical(150)@1.5 = 100");
    // 4K at 2x scaling: physical 7680x4320 -> logical 3840x2160.
    wi::Canvas c4k;
    c4k.init(7680, 4320, 192.0f);
    check(c4k.GetDPIScaling() == 2.0f, "192 DPI -> scale 2.0");
    check(c4k.GetLogicalWidth() == 3840.0f && c4k.GetLogicalHeight() == 2160.0f,
          "4K @2x -> logical 3840x2160");
    std::printf("[wicked] wi::Canvas OK\n");
}

void test_math() {
    std::printf("[wicked] wi::math types…\n");
    XMFLOAT2 v2(1.0f, 2.0f);
    check(v2.x == 1.0f && v2.y == 2.0f, "XMFLOAT2 ctor + access");
    XMFLOAT3 v3(3.0f, 4.0f, 5.0f);
    check(v3.x == 3.0f && v3.y == 4.0f && v3.z == 5.0f, "XMFLOAT3 ctor + access");
    XMFLOAT4 v4(1.0f, 0.0f, 0.0f, 1.0f);
    check(v4.x == 1.0f && v4.w == 1.0f, "XMFLOAT4 ctor + access");
    std::printf("[wicked] wi::math OK\n");
}

}  // namespace

int main() {
    test_color();
    test_canvas();
    test_math();
    if (g_failures == 0) {
        std::printf("[wicked] ALL PASSED\n");
        return 0;
    }
    std::printf("[wicked] %d FAILURE(S)\n", g_failures);
    return 1;
}
