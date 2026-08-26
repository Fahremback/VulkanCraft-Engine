#version 450

// VulkanCraft Stable Adaptive Grid — fragment stage.
//
// Hierarchical CONTINUOUS LOD. The previous version picked one decade
// (fineStep) and crossfaded to the next (coarseStep): the handoff is
// concentrated in a narrow phase band, so the projection draws it as a
// perceptible straight/curved frontier across the viewport, and the two
// partially-faded incommensurate frequencies interfere into dotted
// "starfield" artifacts near the transition.
//
// Instead, every scale decides its own visibility from the real pixel
// footprint (pixelsPerCell) and scales COEXIST: fine lines are strongest near
// the camera, coarser lines are already present underneath and take over
// smoothly as the fine ones retire. There is no decade boundary to see.
//
// Explicit Nyquist rule: a frequency whose cell spans fewer than ~1.5 pixels
// can never be rasterized reliably (partial coverage -> shimmer, moiré,
// isolated dots). gridLine1D fades it out between 1.5 and 4 px/cell and
// returns ZERO below — a subpixel frequency never reaches the fract()/output.
//
// - Constant screen-space line width (gridLine1D) and axes (axisLine).
// - No discard at all: with premultiplied alpha, alpha = 0 is invisible and
//   avoids fragments entering/leaving between frames.
// - Premultiplied alpha output; the grid pipeline blends with
//   ONE / ONE_MINUS_SRC_ALPHA and tests depth but does not write it.
// - NaN/quad safety: no early discard and every division is epsilon-guarded,
//   so the 2x2 quads used by fwidth stay intact.

layout (location = 0) in vec3 nearPoint;
layout (location = 1) in vec3 farPoint;

layout (location = 0) out vec4 outColor;

layout (push_constant) uniform PushConstants {
    mat4 invViewProj;
    vec4 cameraPos;
} pc;

// Cells of the reference scale span roughly this many pixels before the finer
// scales retire (the coarser ones keep going to the horizon). Raise to 96 for
// even earlier retirement.
const float TARGET_PIXELS_PER_CELL = 64.0;

// One family of parallel grid lines with a constant screen-space width.
// fwidth(coord) is the cell size in pixels; below ~1.5 px/cell the frequency
// is explicitly killed (Nyquist), which eliminates shimmer / dots / moiré.
float gridLine1D(float worldCoord, float step, float pixelWidth) {
    float coord = worldCoord / step;
    float fw = max(fwidth(coord), 1e-6);

    // Pixels per cell: 1.0 / fw. Subpixel frequencies simply do not exist —
    // smooth fade between 1.5 and 4 px/cell, hard zero below.
    float pixelsPerCell = 1.0 / fw;
    float frequencyVisibility = smoothstep(1.5, 4.0, pixelsPerCell);

    float d = abs(fract(coord + 0.5) - 0.5);
    float inner = fw * pixelWidth * 0.35;
    float outer = fw * pixelWidth * 1.15;
    float line = 1.0 - smoothstep(inner, outer, d);
    return line * frequencyVisibility;
}

// Complete grid at one scale; X and Z line families handled separately.
float gridAtScale(vec2 worldXZ, float step) {
    float x = gridLine1D(worldXZ.x, step, 1.0);
    float z = gridLine1D(worldXZ.y, step, 1.0);
    return max(x, z);
}

// Visibility of one scale: ramps 0->1 as its cells span enough pixels. The
// lower edge is the same Nyquist kill as gridLine1D (belt and suspenders:
// coarse scales never bleed into the subpixel zone either).
float scaleVisibility(float step, float worldPerPixel) {
    float pixelsPerCell = step / max(worldPerPixel, 1e-8);
    return smoothstep(1.5, 6.0, pixelsPerCell);
}

// Constant screen-width world axis (width in pixels, not meters), so the axis
// stays elegant at any distance.
float axisLine(float worldDistance, float pixelWidth) {
    float fw = max(fwidth(worldDistance), 1e-6);
    float d = abs(worldDistance);
    float inner = fw * pixelWidth * 0.30;
    float outer = fw * pixelWidth * 1.10;
    return 1.0 - smoothstep(inner, outer, d);
}

void main() {
    // ----------------------------------------------------------------------
    // Ray -> ground (Y = 0). No early discard: every fragment runs the same
    // path so the 2x2 quads used by fwidth stay intact.
    //
    // This is an INFINITE ray, not a near->far segment: the fullscreen
    // triangle already projects the camera ray, and extrapolating past the
    // far plane is the same straight line, so the grid never stops at a
    // distance cutoff — it runs to the horizon.
    // ----------------------------------------------------------------------
    vec3 rayOrigin = nearPoint;
    vec3 rayDirection = farPoint - nearPoint;
    float denom = rayDirection.y;
    bool valid = abs(denom) > 1e-6;
    float safeDenom = valid ? denom : 1.0;
    float t = -rayOrigin.y / safeDenom;
    valid = valid && t >= 0.0;
    float safeT = valid ? t : 0.0; // keep the ray math finite for the quad

    vec3 p = rayOrigin + safeT * rayDirection;

    // ----------------------------------------------------------------------
    // How much world does one pixel cover? (screen-space footprint)
    // ----------------------------------------------------------------------
    vec2 dx = dFdx(p.xz);
    vec2 dy = dFdy(p.xz);
    float worldPerPixel = max(max(length(dx), length(dy)), 1e-6);

    // ----------------------------------------------------------------------
    // Hierarchical continuous LOD: evaluate the reference decade and the three
    // coarser ones. Each scale's own pixelsPerCell visibility decides — finer
    // scales retire smoothly as they approach the Nyquist limit, coarser ones
    // are already present underneath (sparse), so the handoff is invisible.
    // log10 is not a GLSL builtin: ln(x) / ln(10).
    // ----------------------------------------------------------------------
    float logarithmicStep = log(max(worldPerPixel * TARGET_PIXELS_PER_CELL, 1e-6)) / 2.302585093;
    float decade = floor(logarithmicStep);

    float grid = 0.0;
    for (int i = 0; i <= 3; ++i) {
        float step = pow(10.0, decade + float(i));
        grid = max(grid, gridAtScale(p.xz, step) * scaleVisibility(step, worldPerPixel));
    }

    // ----------------------------------------------------------------------
    // Horizon: only the extreme grazing angle fades, to fight numerical
    // instability at the exact horizon line. The per-frequency Nyquist fade
    // already retires every frequency before it reaches the subpixel zone, so
    // no radial MAX_DISTANCE cutoff is needed — the grid is infinite.
    // ----------------------------------------------------------------------
    vec3 ray = normalize(farPoint - nearPoint);
    float incidence = abs(ray.y);
    if (isnan(incidence) || isinf(incidence)) incidence = 1.0;
    float horizonFade = smoothstep(0.001, 0.006, incidence);

    // ----------------------------------------------------------------------
    // Visual: the grid gets discretely stronger with distance so it stays
    // readable (darker near the camera, brighter toward the horizon).
    // ----------------------------------------------------------------------
    vec3 gridColor = vec3(0.265, 0.300, 0.385);
    float hierarchy = clamp(decade * 0.25, 0.0, 1.0);
    gridColor = mix(gridColor, vec3(0.345, 0.390, 0.500), hierarchy * 0.45);

    float alpha = grid * 0.72;

    // ----------------------------------------------------------------------
    // Axes at constant screen width: X = red along world X (distance to Z=0),
    // Z = blue along world Z (distance to X=0).
    // ----------------------------------------------------------------------
    float xAxis = axisLine(p.z, 1.55);
    float zAxis = axisLine(p.x, 1.55);

    vec3 color = gridColor;
    color = mix(color, vec3(0.92, 0.24, 0.28), xAxis);
    color = mix(color, vec3(0.27, 0.42, 1.00), zAxis);
    alpha = max(alpha, max(xAxis, zAxis) * 0.92);

    // ----------------------------------------------------------------------
    // Final visibility: valid intersection + distance + horizon fade.
    // No discard — alpha = 0 with premultiplied blending is fully invisible
    // and avoids pixels popping in/out between frames.
    // ----------------------------------------------------------------------
    alpha *= valid ? 1.0 : 0.0;
    alpha *= horizonFade;
    if (isnan(alpha) || isinf(alpha)) alpha = 0.0;
    alpha = max(alpha, 0.0);

    // ----------------------------------------------------------------------
    // Exact depth of the intersection point, remapped from GL [-1, 1] clip
    // space to the Vulkan depth attachment [0, 1]. nearPoint sits at clipZ=-1
    // and farPoint at clipZ=+1, so along the ray ndcZ = -1 + 2t and Vulkan
    // depth = ndcZ * 0.5 + 0.5 = t. (The pipeline tests depth but does not
    // write it, so this is test-only.)
    // ----------------------------------------------------------------------
    gl_FragDepth = clamp(t, 0.0, 1.0);

    // Premultiplied alpha — matches the grid pipeline blend
    // (srcColor = ONE, dstColor = ONE_MINUS_SRC_ALPHA).
    outColor = vec4(color * alpha, alpha);
}
