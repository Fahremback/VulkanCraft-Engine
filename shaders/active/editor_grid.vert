#version 450

// Analytic infinite grid (anti-aliased, distance-faded).
// Draws a single fullscreen triangle (no vertex buffer); each fragment
// intersects its view ray with the ground plane (Y = 0) in the fragment
// shader. The near/far endpoints are unprojected here so the fragment
// can interpolate the ray in world space.

layout (location = 0) out vec3 nearPoint;
layout (location = 1) out vec3 farPoint;

layout (push_constant) uniform PushConstants {
    mat4 view;
    mat4 proj;
} pc;

vec3 unproject(vec2 ndc, float clipZ) {
    // GL clip convention: near = -1, far = +1 (matches glm::perspective).
    mat4 invView = inverse(pc.view);
    mat4 invProj = inverse(pc.proj);
    vec4 p = invView * invProj * vec4(ndc, clipZ, 1.0);
    return p.xyz / p.w;
}

void main() {
    // Fullscreen triangle from the vertex index — covers the whole screen
    // with a single vkCmdDraw(3). Vertices: (-1,-1), (3,-1), (-1,3).
    vec2 ndc = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;
    nearPoint = unproject(ndc, -1.0);
    farPoint  = unproject(ndc,  1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
