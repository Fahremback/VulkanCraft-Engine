#version 450

// Analytic infinite grid (anti-aliased).
// Draws a single fullscreen triangle (no vertex buffer); each fragment
// intersects its view ray with the ground plane (Y = 0) in the fragment
// shader. The inverse view-projection arrives from the CPU (constant for the
// whole draw), so the near/far endpoints are unprojected here cheaply; the
// fragment treats the ray as an infinite ray (no far-plane cutoff) so the
// grid runs to the horizon.

layout (location = 0) out vec3 nearPoint;
layout (location = 1) out vec3 farPoint;

layout (push_constant) uniform PushConstants {
    mat4 invViewProj;
    mat4 viewProj;
} pc;

vec3 unproject(vec2 ndc, float clipZ) {
    // Vulkan raster depth convention: visible clip Z = 0..1.  Match the
    // scene viewport exactly so the analytic plane cannot drift from meshes.
    //
    // BUG-EDITOR-GRID-006 (vertical flip mismatch): the editor's projection
    // matrix is OpenGL-style Y-UP (glm::perspective without proj[1][1]*=-1),
    // and the rest of the viewport unprojects screen NDC with the Y flipped
    // to match (see unproject_to_plane / pick ray: ndcY = 1.0 - y*2, the
    // Y-down Vulkan DPI v.s. the Y-up projection). This grid built its NDC
    // fullscreen triangle with Y-UP positions ((-1,-1),(3,-1),(-1,3)) and fed
    // that straight into invViewProj, so every ray's Y left inverted relative
    // to the scene: the grid slanted as a "runway" toward one corner and
    // drifted off the (correctly unprojected) entities on orbit. Flip ndc.y
    // here so the analytic plane uses the same Y convention as the picking /
    // gizmo unproject.
    vec4 p = pc.invViewProj * vec4(ndc.x, -ndc.y, clipZ, 1.0);
    return p.xyz / p.w;
}

void main() {
    // Fullscreen triangle from the vertex index — covers the whole screen
    // with a single vkCmdDraw(3). Vertices: (-1,-1), (3,-1), (-1,3).
    vec2 ndc = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;
    nearPoint = unproject(ndc, 0.0);
    vec3 farWorld = unproject(ndc, 1.0);

    // A1-G-GRID-RIGHT-SKEW-UNDERSIDE (right warp / projective skew): hand the
    // rasterizer the PROJECTIVE, UN-normalized ray vector `farWorld - nearPoint`,
    // reduced only by one fixed, constant scale. The ray through nearPoint and
    // farWorld is the SAME line no matter how long it is, and scaling a vector
    // by a *constant* does not change its direction — but normalizing it at
    // each vertex DOES bend the direction field, because normalize() is
    // nonlinear. The fullscreen triangle has its far offscreen vertex at
    // (3,-1), so interpolating three already-normalized per-vertex directions
    // between the corners bends the projected plane and the error is largest on
    // the right side of the camera. Passing the un-normalized vector scaled by
    // one fixed, safe constant (a) keeps interpolation affine/linear so every
    // pixel gets the exact same straight line its vertex endpoints define, and
    // (b) pins the interpolant's magnitude to a modest, fp32-precise range so
    // interpolation toward the screen centre does not sum huge OPPOSITE corner
    // values with catastrophic cancellation (BUG-EDITOR-GRID-005 orbit shiver).
    // The fragment re-normalizes the interpolated vector before intersecting
    // the plane — the ONLY normalization, done after interpolation.
    vec3 dir = farWorld - nearPoint;
    const float RAY_SCALE = 1.0 / 24.0; // fixed, constant; any safe scale keeps the ray
    farPoint = nearPoint + dir * RAY_SCALE;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
