#version 450

// Analytic infinite grid (anti-aliased).
// Draws a single fullscreen triangle (no vertex buffer); each fragment
// intersects its view ray with the ground plane (Y = 0) in the fragment
// shader. The inverse view-projection arrives from the CPU (constant for the
// whole draw), so the near/far endpoints are unprojected here cheaply; the
// fragment treats the ray as an infinite ray (no far-plane cutoff) so the
// grid runs to the horizon.

layout (location = 0) out vec3 nearPoint;
layout (location = 1) out vec3 rayVector;
layout (location = 2) flat out vec2 gridOrigin;

layout (push_constant) uniform PushConstants {
    mat4 invViewProj;
    mat4 viewProj;
} pc;

vec3 unproject(vec2 ndc, float clipZ) {
    // Vulkan raster depth convention: visible clip Z = 0..1.  Match the
    // scene viewport exactly so the analytic plane cannot drift from meshes.
    //
    // GROUND TRUTH (BUG-EDITOR-GRID-006 RESOLVED CORRECTLY): the editor's
    // projection is OpenGL-style Y-UP (glm::perspective without proj[1][1]*=-1)
    // and the fullscreen triangle below stamps gl_Position from the SAME `ndc`
    // this function unprojects. A fragment is rasterized at clip `ndc`, so its
    // grid ray must be invViewProj * vec4(ndc.x,  ndc.y, ...) — the SAME
    // unflipped convention the pick ray (viewport_mouse_dir: ndcY = 1 - y*2,
    // then invViewProj * vec4(ndcX, ndcY, ...)) and the mesh vertex shader use.
    // The earlier fix negated Y here (`-ndc.y`), which mirrored every grid ray
    // vertically against the meshes: the grid drew the plane upside down
    // ("grade invertida"), the camera read mirrored rays ("nascendo debaixo" /
    // camera bugged on it), and only the origin (Y-symmetric) looked right.
    // Probe GridPickConsistencyTests: unflipped recovers 6/6 on-screen ground
    // points per camera; the `-ndc.y` mirror recovers only the origin. This is
    // the SAME +ndc.y the pick uses, so the analytic plane aligns with meshes.
    vec4 p = pc.invViewProj * vec4(ndc.x,  ndc.y, clipZ, 1.0);
    return p.xyz / p.w;
}

void main() {
    // Fullscreen triangle from the vertex index — covers the whole screen
    // with a single vkCmdDraw(3). Vertices: (-1,-1), (3,-1), (-1,3).
    vec2 ndc = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;
    nearPoint = unproject(ndc, 0.0);

    // VIS-GRID-001: never reconstruct the ray from clipZ=1.0. With the editor's
    // 50 km far plane that endpoint is tens of thousands of world units away,
    // so a slow orbit changes its fp32 representation by visible world-space
    // amounts. Any two points on the same projective ray define the exact same
    // line, therefore clipZ=0.5 gives us a short, camera-local delta with far
    // more mantissa left for interpolation. Keep it UN-normalized here:
    // normalization is nonlinear and would bend the direction field across the
    // oversized fullscreen triangle. The fragment normalizes once, after
    // interpolation.
    vec3 localPoint = unproject(ndc, 0.5);
    rayVector = localPoint - nearPoint;

    // Camera-relative phase rebasing. The reference is computed from the centre
    // near-plane point, so all three fullscreen vertices publish the SAME flat
    // value. 10240 is an exact multiple of both visible grid periods (1 m and
    // 10 m), therefore changing rebase cells cannot move either grid family.
    // Keeping grid arithmetic near zero avoids large-world fract/fwidth jitter.
    const float GRID_REBASE_PERIOD = 10240.0;
    vec3 centerNear = unproject(vec2(0.0), 0.0);
    gridOrigin = floor(centerNear.xz / GRID_REBASE_PERIOD) * GRID_REBASE_PERIOD;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
