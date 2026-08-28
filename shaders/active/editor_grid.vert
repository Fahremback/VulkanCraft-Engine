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
    vec4 p = pc.invViewProj * vec4(ndc, clipZ, 1.0);
    return p.xyz / p.w;
}

void main() {
    // Fullscreen triangle from the vertex index — covers the whole screen
    // with a single vkCmdDraw(3). Vertices: (-1,-1), (3,-1), (-1,3).
    vec2 ndc = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;
    nearPoint = unproject(ndc, 0.0);
    farPoint  = unproject(ndc, 1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
