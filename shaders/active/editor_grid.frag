#version 450

// VulkanCraft Stable Adaptive Grid — fragment stage.
//
// Instead of drawing 1 m / 10 m / 100 m simultaneously (where a fixed
// frequency can always sit at its resolution limit near the grazing angle and
// shimmer between frames), the grid frequency is chosen automatically from the
// real pixel footprint on the ground: 1m -> 10m -> 100m -> 1000m, with a
// smooth crossfade between the two decades involved in each transition.
//
// A subpixel frequency never survives: when a cell spans fewer than
// TARGET_PIXELS_PER_CELL pixels, the grid switches to the next decade long
// before entering the Nyquist danger zone.
//
// - Constant screen-space line width (gridLine1D) and axes (axisLine).
// - No discard at all: with premultiplied alpha, alpha = 0 is invisible and
//   avoids fragments entering/leaving between frames. (Depth writes are
//   disabled on the pipeline, so the gl_FragDepth below is test-only.)
// - Premultiplied alpha output; the grid pipeline blends with
//   ONE / ONE_MINUS_SRC_ALPHA and tests depth but does not write it.
//
// NaN/quad safety (black-block fix): no early discard and every division is
// epsilon-guarded, so the 2x2 quads used by fwidth stay intact.

layout (location = 0) in vec3 nearPoint;
layout (location = 1) in vec3 farPoint;

layout (location = 0) out vec4 outColor;

layout (push_constant) uniform PushConstants {
    mat4 view;
    mat4 proj;
} pc;

// Cells of the current grid frequency span roughly this many pixels before the
// grid steps up to the next decade. Raise to 96 for even earlier transitions.
const float TARGET_PIXELS_PER_CELL = 64.0;

// One family of parallel grid lines with a constant screen-space width.
float gridLine1D(float worldCoord, float step, float pixelWidth) {
    float coord = worldCoord / step;
    float fw = max(fwidth(coord), 1e-6);
    float d = abs(fract(coord + 0.5) - 0.5);
    float inner = fw * pixelWidth * 0.35;
    float outer = fw * pixelWidth * 1.15;
    return 1.0 - smoothstep(inner, outer, d);
}

// Complete grid at one scale; X and Z line families handled separately.
float gridAtScale(vec2 worldXZ, float step) {
    float x = gridLine1D(worldXZ.x, step, 1.0);
    float z = gridLine1D(worldXZ.y, step, 1.0);
    return max(x, z);
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
    // Automatic scale selection. We want roughly TARGET_PIXELS_PER_CELL for
    // the current grid, so the desired step is worldPerPixel * that target.
    // A log10 decomposition gives the decade (fineStep) and the position
    // within it (phase); the transition crossfades fine -> coarse = fine*10.
    // ----------------------------------------------------------------------
    float desiredStep = worldPerPixel * TARGET_PIXELS_PER_CELL;
    // log10 is not a GLSL builtin: ln(x) / ln(10).
    float logarithmicStep = log(max(desiredStep, 1e-6)) / 2.302585093;
    float decade = floor(logarithmicStep);
    float phase = fract(logarithmicStep);

    float fineStep = pow(10.0, decade);
    float coarseStep = fineStep * 10.0;

    // Crossfade: while the fine grid loses resolution, the coarse one takes
    // over. At the moment of the decade change the previous coarse is exactly
    // the new fine, so no frequency pops in or out.
    float transition = smoothstep(0.48, 0.92, phase);

    float fineGrid = gridAtScale(p.xz, fineStep);
    float coarseGrid = gridAtScale(p.xz, coarseStep);
    float grid = mix(fineGrid, coarseGrid, transition);

    // ----------------------------------------------------------------------
    // Horizon: only the extreme grazing angle fades, to fight numerical
    // instability at the exact horizon line. The adaptive LOD (fine/coarse
    // decade crossfade above) already keeps the grid readable at any distance,
    // so no radial MAX_DISTANCE cutoff is needed — the grid is infinite.
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
    float hierarchy = transition;
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
    // space to the Vulkan depth attachment [0, 1]. The pipeline tests depth
    // but does not write it.
    // ----------------------------------------------------------------------
    vec4 clip = pc.proj * pc.view * vec4(p, 1.0);
    float safeW = max(abs(clip.w), 1e-6);
    float ndcZ = clip.z / safeW * sign(clip.w);
    gl_FragDepth = clamp(ndcZ * 0.5 + 0.5, 0.0, 1.0);

    // Premultiplied alpha — matches the grid pipeline blend
    // (srcColor = ONE, dstColor = ONE_MINUS_SRC_ALPHA).
    outColor = vec4(color * alpha, alpha);
}
