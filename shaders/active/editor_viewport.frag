#version 450

layout (location = 0) in vec3 fragColor;
layout (location = 1) in vec3 fragNormal;
layout (location = 0) out vec4 outColor;

void main() {
    vec3 n = normalize(fragNormal);
    vec3 lightDir = normalize(vec3(0.45, 0.85, 0.55));
    float shade = 0.5 + 0.5 * max(dot(n, lightDir), 0.0);
    outColor = vec4(fragColor * shade, 1.0);
}
