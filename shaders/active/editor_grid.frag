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
    mat4 viewProj;
} pc;

const float MINOR_GRID_STEP = 1.0;
const float MAJOR_GRID_STEP = 10.0;

// One family of parallel grid lines with a constant screen-space width.
// fwidth(coord) is the cell size in pixels; below ~1.5 px/cell the frequency
// is explicitly killed (Nyquist), which eliminates shimmer / dots / moiré.
//
// BUG-EDITOR-GRID-004 (motion throb / "samba"): a ~1.5 px line that translates
// ~1 px per frame (ANY camera drag — the pattern speed in px/frame is
// independent of distance for orbit rotations) re-rolls its subpixel phase
// every frame, so every discrete line shimmered during motion. Two mitigations
// that keep the static look intact:
//  1. Wider analytic AA (0.50..1.50 px instead of 0.35..1.15) — the intensity
//     of a wider profile changes less when its phase shifts by a fraction of
//     a pixel.
//  2. Fade-to-mean: once the cell drops below ~6 px/cell the line profile
//     converges to its exact cell-average coverage (inner+outer), so the dense
//     far field becomes a STABLE gradient with no discrete structure left to
//     alias. The average brightness is unchanged versus the discrete lines
//     (same coverage), and the existing frequency retirement still fades the
//     mean out below 1.5 px/cell.
float gridLine1D(float worldCoord, float step, float pixelWidth) {
    float coord = worldCoord / step;
    float fw = max(fwidth(coord), 1e-6);

    float d = abs(fract(coord + 0.5) - 0.5);
    float inner = fw * 0.50;
    float outer = fw * 1.50;
    float line = 1.0 - smoothstep(inner, outer, d);

    // Converge to the cell-average coverage (inner+outer, derived by
    // integrating the profile over one cell) as cells become dense. Beyond
    // ~6 px/cell the lines stay crisp; below ~2 px/cell the output is the
    // pure mean — motion-stable by construction.
    float mean = clamp(inner + outer, 0.0, 1.0);
    float toMean = smoothstep(0.15, 0.45, fw);
    line = mix(line, mean, toMean);

    // Pixels per cell: 1.0 / fw. Subpixel frequencies simply do not exist —
    // smooth fade between 1.5 and 4 px/cell, hard zero below.
    float pixelsPerCell = 1.0 / fw;
    float frequencyVisibility = smoothstep(1.5, 4.0, pixelsPerCell);

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
// stays elegant at any distance. Wider AA than the grid lines because the
// axes pivot around the orbit target during yaw — the same motion-throb
// mitigation as gridLine1D (BUG-EDITOR-GRID-004).
float axisLine(float worldDistance, float pixelWidth) {
    float fw = max(fwidth(worldDistance), 1e-6);
    float d = abs(worldDistance);
    float inner = fw * pixelWidth * 0.45;
    float outer = fw * pixelWidth * 1.45;
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
    // BUG-EDITOR-GRID-005 (orbit jitter): the ray target is unprojected from
    // clipZ = 1.0, i.e. the FULL far plane (farPlane = 50000 by default). That
    // target can sit tens of thousands of units away, and fp32 keeps only ~1e-7
    // RELATIVE precision there. Rotating the camera perturbs the far corners by
    // a few ULPs of a huge number, and subtracting farPoint - nearPoint spreads
    // that absolute error into every component of the intersection point -- so
    // the analytic grid shivers while meshes (which project precise local
    // vertices) stay rock solid. Fix: normalize the direction FIRST and do all
    // of the remaining math on a unit vector, so the magnitude-50000 error is
    // confined to the initial normalize instead of leaking into t and p.
    vec3 rawDir = farPoint - nearPoint;
    float rawLen = length(rawDir);
    vec3 rayDirection = rawLen > 1e-9 ? (rawDir / rawLen) : vec3(0.0, -1.0, 0.0);

    // A1-G-GRID-RIGHT-SKEW-UNDERSIDE (underside face): the grid plane only
    // exists on the side facing UP. A ray striking Y=0 from BELOW (camera
    // under the plane looking up) must produce NO grid. The plane is only
    // visible when the view ray is travelling downward, i.e. its Y component
    // is strictly negative. Requiring rayDirection.y < -epsilon rejects the
    // underside entirely; those fragments get zero alpha and background depth
    // (handled by `valid` below). t >= 0 keeps the intersection in front of
    // the camera.
    float denom = rayDirection.y;
    bool above = denom < -1e-6;            // striking from above -> ray points down
    float safeDenom = above ? denom : -1.0;
    float t = -rayOrigin.y / safeDenom;
    bool valid = above && t >= 0.0;
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
    // Reuse the direction normalized above (identical ray, no second
    // large-difference normalize).
    vec3 ray = rayDirection;
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
    // Exact depth of the world-space intersection.  The previous version
    // wrote the ray parameter `t` as depth; t is measured in world units and
    // is not clip-space depth, so it caused z-fighting/shimmer whenever the
    // camera moved slowly.  Project the actual point with the same matrix as
    // the scene.  Vulkan raster depth is already clip.z / clip.w (0..1).
    // ----------------------------------------------------------------------
    // Depth derived EXACTLY from the scene's view-projection (A1-G: grid depth
    // participation). Underside/invalid fragments write the background depth so
    // they never occlude the world and produce no grid below the plane.
    vec4 clipPoint = pc.viewProj * vec4(p, 1.0);
    float safeClipW = abs(clipPoint.w) > 1e-6 ? clipPoint.w : 1.0;
    float gridDepth = clipPoint.z / safeClipW;
    if (!valid || isnan(gridDepth) || isinf(gridDepth)) {
        // Background depth: no grid, no occlusion from below.
        gl_FragDepth = 1.0;
        // Premultiplied alpha — matches the grid pipeline blend
        // (srcColor = ONE, dstColor = ONE_MINUS_SRC_ALPHA).
        outColor = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    // ----------------------------------------------------------------------
    // BUG-EDITOR-GRID-003 (coplanar tie dance): geometry that lies EXACTLY on
    // the grid plane (terrain falloff ring at Y=0, entity bottom faces) and
    // the analytic grid depth agree only to ~2-3 fp32 ULPs — they come from
    // different rounding chains (vertex chain + raster interpolation vs the
    // per-pixel ray chain here). With LEQUAL every such pixel is a coin flip
    // whose pattern re-rolls on every camera rotation: the grid visibly
    // "dances" over flat ground while meshes stay stable. The rasterizer
    // depth bias CANNOT fix this (it is applied to the interpolated depth,
    // which gl_FragDepth overrides), so the nudge lives here instead: pull
    // the grid depth toward the camera by ~10 ULPs of its own magnitude.
    // That is far above the tie noise (~3 ULPs), so the grid wins every
    // coplanar contact deterministically, while in world units the offset is
    // sub-millimeter near the camera and stays inside the depth-tie band at
    // any range — never visible against geometry that is actually in front.
    // ----------------------------------------------------------------------
    gridDepth -= gridDepth * 1.2e-6;
    if (isnan(gridDepth) || isinf(gridDepth)) gridDepth = 1.0;
    gl_FragDepth = clamp(gridDepth, 0.0, 1.0);

    // Premultiplied alpha — matches the grid pipeline blend
    // (srcColor = ONE, dstColor = ONE_MINUS_SRC_ALPHA).
    outColor = vec4(color * alpha, alpha);
}
