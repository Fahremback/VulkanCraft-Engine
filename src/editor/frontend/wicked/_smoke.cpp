// Compile smoke for the ported Wicked GUI base layer (frontend/wicked).
// Verifies the self-contained types compile against DirectXMath on MSVC.
// This TU is not part of the engine build — it exists to validate the base.
#include "CommonInclude.h"
#include "wiVector.h"
#include "wiColor.h"
#include "wiMath.h"
#include "wiCanvas.h"

int wicked_base_smoke() {
    const wi::Color c(60, 60, 60, 200);
    const wi::Color blended = wi::Color::lerp(c, wi::Color(255, 255, 255, 255), 0.5f);
    wi::Canvas canvas;
    canvas.init(1920, 1080, 96);
    const float lw = canvas.GetLogicalWidth();
    const float dpiscale = canvas.GetDPIScaling();
    const uint32_t phys = canvas.LogicalToPhysical(lw);
    (void)blended;
    (void)phys;
    (void)dpiscale;
    return 0;
}
