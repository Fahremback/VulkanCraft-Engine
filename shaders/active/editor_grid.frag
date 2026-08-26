#version 450

// VulkanCraft Stable World Grid — fragment stage.
//
// The grid is anchored permanently to world units: one minor line per metre
// and one major line every ten metres. Camera distance never changes the set
// of scales, so there is no LOD frontier, density band or colour step. Each
// fixed frequency only fades when its projected footprint approaches Nyquist.
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

const float MINOR_GRID_STEP = 1.0;
const float MAJOR_GRID_STEP = 10.0;

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

// Visibility of the complete square cell, shared by both line orientations.
// Using the largest 2D footprint is intentionally conservative: a cell retires
// as a unit when either projected dimension becomes undersampled. This avoids
// the triangular holes where oblique lines vanished while radial lines lived on.
float cellVisibility(float step, float worldPerPixel) {
    float pixelsPerCell = step / max(worldPerPixel, 1e-8);
    return smoothstep(1.5, 5.0, pixelsPerCell);
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

    // Full projected footprint of the ground cell. This is shared by X lines,
    // Z lines and axes so their fade cannot disagree by viewing orientation.
    float worldPerPixel = max(max(length(dFdx(p.xz)), length(dFdy(p.xz))), 1e-6);

    // Fixed world hierarchy. Both families exist everywhere and are exact
    // multiples, so their intersections remain aligned while fwidth performs
    // the only distance-dependent operation: a gradual anti-aliasing fade.
    float minorGrid = gridAtScale(p.xz, MINOR_GRID_STEP)
                    * cellVisibility(MINOR_GRID_STEP, worldPerPixel);
    float majorGrid = gridAtScale(p.xz, MAJOR_GRID_STEP)
                    * cellVisibility(MAJOR_GRID_STEP, worldPerPixel);

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
    // Stable colours and strengths. Major lines are deliberately only a little
    // stronger than minor lines; they identify the fixed 10 m hierarchy without
    // turning into a second oversized grid at distance.
    // ----------------------------------------------------------------------
    vec3 minorColor = vec3(0.255, 0.290, 0.365);
    vec3 majorColor = vec3(0.315, 0.355, 0.445);
    float minorAlpha = minorGrid * 0.46;
    float majorAlpha = majorGrid * 0.58;
    float alpha = max(minorAlpha, majorAlpha);
    float majorMix = majorAlpha / max(alpha, 1e-6);
    vec3 gridColor = mix(minorColor, majorColor, majorMix);

    // ----------------------------------------------------------------------
    // Axes at constant screen width: X = red along world X (distance to Z=0),
    // Z = blue along world Z (distance to X=0).
    // ----------------------------------------------------------------------
    // Axes belong to the local editing grid. Fading them with the minor cells
    // prevents a single coloured ray from surviving alone to the vanishing point.
    float axisVisibility = cellVisibility(MINOR_GRID_STEP, worldPerPixel);
    float xAxis = axisLine(p.z, 1.55) * axisVisibility;
    float zAxis = axisLine(p.x, 1.55) * axisVisibility;

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
