#version 450

layout (location = 0) in vec2 fragUV;
layout (location = 0) out vec4 outColor;

layout (push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 cameraPos;
    vec4 sunDirection;
    vec4 sunColor;
    vec4 environment;
} push;

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float noise2(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i), hash21(i + vec2(1, 0)), f.x),
               mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1)), f.x), f.y);
}

float fbm(vec2 p) {
    float value = 0.0, amplitude = 0.5;
    for (int i = 0; i < 5; ++i) {
        value += noise2(p) * amplitude;
        p = p * 2.03 + vec2(17.1, 9.2);
        amplitude *= 0.5;
    }
    return value;
}

void main() {
    vec2 ndc = fragUV * 2.0 - 1.0;
    vec4 farWorld = inverse(push.mvp) * vec4(ndc, 1.0, 1.0);
    vec3 ray = normalize(farWorld.xyz / farWorld.w - push.cameraPos.xyz);
    vec3 sunDir = normalize(push.sunDirection.xyz);
    float daylight = push.environment.y;
    float horizon = clamp(ray.y * 4.0 + 0.18, 0.0, 1.0);
    float sunset = (1.0 - smoothstep(0.02, 0.38, abs(sunDir.y))) * daylight;

    vec3 nightZenith = vec3(0.002, 0.006, 0.022);
    vec3 nightHorizon = vec3(0.018, 0.032, 0.070);
    vec3 dayZenith = vec3(0.08, 0.29, 0.72);
    vec3 dayHorizon = mix(vec3(0.48, 0.69, 0.91), vec3(1.0, 0.28, 0.06), sunset * 0.78);
    vec3 sky = mix(mix(nightHorizon, nightZenith, horizon),
                   mix(dayHorizon, dayZenith, pow(horizon, 0.38)), daylight);

    float sunAmount = max(dot(ray, sunDir), 0.0);
    float sunDisc = smoothstep(0.99942, 0.99978, sunAmount) * daylight;
    float sunGlow = pow(sunAmount, 320.0) * daylight + pow(sunAmount, 18.0) * 0.16 * daylight;
    sky += mix(vec3(1.0, 0.31, 0.06), vec3(1.0, 0.93, 0.70), smoothstep(0.02, 0.45, sunDir.y))
         * (sunDisc * 12.0 + sunGlow * 2.8);

    vec3 moonDir = -sunDir;
    float moonAmount = max(dot(ray, moonDir), 0.0);
    float moonDisc = smoothstep(0.99950, 0.99976, moonAmount) * (1.0 - daylight);
    float moonTexture = 0.74 + noise2(ray.xz * 540.0 + ray.y * 113.0) * 0.26;
    sky += vec3(0.62, 0.75, 1.0) * moonDisc * moonTexture * 3.4;
    sky += vec3(0.20, 0.30, 0.52) * pow(moonAmount, 80.0) * (1.0 - daylight) * 0.18;

    if (ray.y > 0.015) {
        vec2 starGrid = vec2(atan(ray.z, ray.x), asin(ray.y)) * vec2(510.0, 420.0);
        vec2 starCell = floor(starGrid);
        vec2 starLocal = fract(starGrid) - 0.5;
        float seed = hash21(starCell);
        vec2 offset = vec2(hash21(starCell + 11.7), hash21(starCell + 37.2)) - 0.5;
        starLocal -= offset * 0.58;

        // A maioria é poeira estelar quase pontual; só uma fração recebe halo ou difração.
        float population = step(0.9974, seed);
        float rareBright = step(0.99955, seed);
        float radius = mix(0.055, 0.16, pow(hash21(starCell + 3.1), 7.0));
        vec2 distortion = vec2(0.78 + hash21(starCell + 5.6) * 0.55,
                               0.78 + hash21(starCell + 8.9) * 0.55);
        float irregularity = 1.0 + 0.20 * sin(atan(starLocal.y, starLocal.x) *
                            (3.0 + floor(hash21(starCell + 19.0) * 4.0)) + seed * 23.0);
        float starCore = 1.0 - smoothstep(radius * 0.45, radius,
                                         length(starLocal * distortion) * irregularity);
        float horizontalRay = exp(-abs(starLocal.y) * 48.0) * exp(-abs(starLocal.x) * 8.0);
        float verticalRay = exp(-abs(starLocal.x) * 48.0) * exp(-abs(starLocal.y) * 8.0);
        float diffraction = (horizontalRay + verticalRay) * rareBright * 0.24;
        float twinkle = 0.82 + 0.18 * sin(push.environment.x * (0.55 + seed) + seed * 31.0);
        float temperature = hash21(starCell + 71.4);
        vec3 starColor = mix(vec3(1.0, 0.78, 0.60), vec3(0.66, 0.80, 1.0), temperature);
        sky += starColor * population * (starCore + diffraction) * twinkle *
               (1.0 - daylight) * smoothstep(0.02, 0.24, ray.y) * mix(1.0, 2.8, rareBright);

        float cloudDistance = (220.0 - push.cameraPos.y) / max(ray.y, 0.02);
        vec2 cloudPos = (push.cameraPos.xz + ray.xz * cloudDistance) * 0.0032
                      + vec2(push.environment.x * 0.0028, push.environment.x * 0.0011);
        float cloud = 0.0;
        float transmittance = 1.0;
        for (int layer = 0; layer < 6; ++layer) {
            float layerDepth = float(layer) / 5.0;
            vec2 layerPos = cloudPos + ray.xz * layerDepth * 0.17 + vec2(layerDepth * 7.3, -layerDepth * 4.1);
            float density = fbm(layerPos) * 0.66 + fbm(layerPos * 2.7 + 19.0) * 0.34;
            density = smoothstep(0.53 + layerDepth*0.018, 0.70, density) * 0.30;
            cloud += density * transmittance;
            transmittance *= 1.0 - density;
        }
        cloud *= smoothstep(0.02, 0.18, ray.y);
        float silver = pow(clamp(dot(ray, sunDir), 0.0, 1.0), 18.0);
        vec3 cloudColor = mix(vec3(0.055, 0.075, 0.12), vec3(0.82, 0.87, 0.92), daylight);
        cloudColor += vec3(1.0, 0.58, 0.25) * sunset * 0.42 + silver * daylight * vec3(0.9, 0.72, 0.45);
        sky = mix(sky, cloudColor, cloud * 0.72);
    }

    float haze = pow(1.0 - abs(ray.y), 10.0);
    sky += mix(vec3(0.015, 0.025, 0.05), vec3(0.32, 0.48, 0.62), daylight) * haze * 0.34;
    outColor = vec4(sky, 1.0);
}
