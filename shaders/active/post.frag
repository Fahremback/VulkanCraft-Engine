#version 450

layout (location = 0) in vec2 fragUV;
layout (location = 0) out vec4 outColor;
layout (binding = 0) uniform sampler2D hdrScene;
layout (binding = 1) uniform sampler2D sceneDepth;
layout (binding = 2) uniform sampler2D minimapScene;
layout (binding = 3) uniform sampler2DArray blockTextures;

layout (push_constant) uniform PostPushConstants {
    vec4 sunScreen;
    vec4 frame;
    vec4 ui;
    vec4 hud;
    vec4 cameraMotion;
    vec4 settings;
} push;

float luminance(vec3 color) { return dot(color, vec3(0.2126, 0.7152, 0.0722)); }
float hash12(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }

float linearViewDepth(float deviceDepth) {
    const float nearPlane = 0.1;
    const float farPlane = 3500.0;
    float ndcDepth = deviceDepth * 2.0 - 1.0;
    return (2.0 * nearPlane * farPlane) /
           max(farPlane + nearPlane - ndcDepth * (farPlane - nearPlane), 0.0001);
}

vec3 aces(vec3 color) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

float roundedBox(vec2 point, vec2 halfSize, float radius) {
    vec2 q = abs(point) - halfSize + radius;
    float distanceToBox = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
    return 1.0 - smoothstep(0.0, 0.0025, distanceToBox);
}

float diamondShape(vec2 p, vec2 halfSize) {
    vec2 q = abs(p) / max(halfSize, vec2(0.00001));
    float d = q.x + q.y;
    return 1.0 - smoothstep(0.93, 1.04, d);
}

float ringStroke(vec2 p, float radius, float thickness) {
    float d = abs(length(p) - radius);
    return 1.0 - smoothstep(thickness, thickness + 0.055, d);
}

float heartShape(vec2 p) {
    float leftLobe = 1.0 - smoothstep(0.34, 0.39, length(p - vec2(-0.25, 0.18)));
    float rightLobe = 1.0 - smoothstep(0.34, 0.39, length(p - vec2(0.25, 0.18)));
    float triangleWidth = clamp((p.y + 0.58) * 0.92, 0.0, 0.58);
    float triangle = step(-0.58, p.y) * step(p.y, 0.18) *
                     (1.0 - smoothstep(triangleWidth - 0.04, triangleWidth, abs(p.x)));
    return max(max(leftLobe, rightLobe), triangle);
}

float foodShape(vec2 p) {
    p = mat2(0.94,-0.342,0.342,0.94) * p;
    float meat = 1.0-smoothstep(0.48,0.54,length((p-vec2(-0.16,0.08))*vec2(0.78,1.0)));
    float bone = roundedBox(p-vec2(0.27,-0.13),vec2(0.30,0.11),0.08);
    float tip = max(1.0-smoothstep(0.14,0.18,length(p-vec2(0.55,-0.23))),
                    1.0-smoothstep(0.14,0.18,length(p-vec2(0.56,-0.04))));
    return max(meat,max(bone,tip));
}
float foodBone(vec2 p) {
    p = mat2(0.94,-0.342,0.342,0.94) * p;
    float shaft=roundedBox(p-vec2(.31,-.13),vec2(.24,.065),.05);
    float tips=max(1.0-smoothstep(.10,.14,length(p-vec2(.57,-.22))),1.0-smoothstep(.10,.14,length(p-vec2(.58,-.04))));
    return max(shaft,tips);
}

float breadShape(vec2 p) {
    p = mat2(0.906, -0.423, 0.423, 0.906) * p;
    vec2 q = p / vec2(0.68, 0.43);
    float loaf = 1.0 - smoothstep(0.93, 1.0, dot(q, q));
    float heel = 1.0 - smoothstep(0.25, 0.31, length((p - vec2(-0.48, 0.0)) / vec2(0.62, 1.0)));
    return max(loaf, heel);
}

float breadScore(vec2 p, float offset) {
    p = mat2(0.906, -0.423, 0.423, 0.906) * p;
    return roundedBox(p - vec2(offset, 0.04), vec2(0.035, 0.24), 0.025);
}

int digitRow(int digit, int row) {
    if (digit == 0) return int[7](14,17,19,21,25,17,14)[row];
    if (digit == 1) return int[7](4,12,4,4,4,4,14)[row];
    if (digit == 2) return int[7](14,17,1,2,4,8,31)[row];
    if (digit == 3) return int[7](14,17,1,6,1,17,14)[row];
    if (digit == 4) return int[7](2,6,10,18,31,2,2)[row];
    if (digit == 5) return int[7](31,16,16,30,1,17,14)[row];
    if (digit == 6) return int[7](14,16,16,30,17,17,14)[row];
    if (digit == 7) return int[7](31,1,2,4,8,8,8)[row];
    if (digit == 8) return int[7](14,17,17,14,17,17,14)[row];
    return int[7](14,17,17,15,1,1,14)[row];
}

float drawDigit(vec2 uv, vec2 center, float pixelSize, int digit) {
    vec2 local = (uv - (center - vec2(2.5, 3.5) * pixelSize)) / pixelSize;
    int pixelX = int(floor(local.x));
    int pixelY = int(floor(local.y));
    if (pixelX < 0 || pixelX >= 5 || pixelY < 0 || pixelY >= 7) return 0.0;
    int rowMask = digitRow(digit, 6 - pixelY);
    return float((rowMask >> (4 - pixelX)) & 1);
}

int glyphRow(int character, int row) {
    if (character == 65) return int[7](14,17,17,31,17,17,17)[row]; // A
    if (character == 67) return int[7](14,17,16,16,16,17,14)[row]; // C
    if (character == 68) return int[7](30,17,17,17,17,17,30)[row]; // D
    if (character == 69) return int[7](31,16,16,30,16,16,31)[row]; // E
    if (character == 70) return int[7](31,16,16,30,16,16,16)[row]; // F
    if (character == 71) return int[7](14,17,16,23,17,17,14)[row]; // G
    if (character == 72) return int[7](17,17,17,31,17,17,17)[row]; // H
    if (character == 73) return int[7](31,4,4,4,4,4,31)[row];       // I
    if (character == 75) return int[7](17,18,20,24,20,18,17)[row]; // K
    if (character == 76) return int[7](16,16,16,16,16,16,31)[row]; // L
    if (character == 77) return int[7](17,27,21,21,17,17,17)[row]; // M
    if (character == 78) return int[7](17,25,21,19,17,17,17)[row]; // N
    if (character == 79) return int[7](14,17,17,17,17,17,14)[row]; // O
    if (character == 80) return int[7](30,17,17,30,16,16,16)[row]; // P
    if (character == 82) return int[7](30,17,17,30,20,18,17)[row]; // R
    if (character == 83) return int[7](15,16,16,14,1,1,30)[row];   // S
    if (character == 84) return int[7](31,4,4,4,4,4,4)[row];       // T
    if (character == 85) return int[7](17,17,17,17,17,17,14)[row]; // U
    if (character == 86) return int[7](17,17,17,17,17,10,4)[row];  // V
    if (character == 88) return int[7](17,17,10,4,10,17,17)[row];  // X
    if (character == 89) return int[7](17,17,10,4,4,4,4)[row];     // Y
    if (character == 90) return int[7](31,1,2,4,8,16,31)[row];     // Z
    return 0;
}

int wordLength(int word) {
    if (word == 0) return 7; // PAUSADO
    if (word == 1) return 9; // CONTINUAR
    if (word == 2) return 8; // GRAFICOS
    if (word == 3) return 4; // SAIR
    if (word == 4) return 6; // CINEMA
    if (word == 5) return 3; // DOF
    if (word == 6) return 6; // VOLTAR
    return 6;                // CHUNKS
}

int wordCharacter(int word, int index) {
    if (word == 0) return int[7](80,65,85,83,65,68,79)[index];
    if (word == 1) return int[9](67,79,78,84,73,78,85,65,82)[index];
    if (word == 2) return int[8](71,82,65,70,73,67,79,83)[index];
    if (word == 3) return int[4](83,65,73,82)[index];
    if (word == 4) return int[6](67,73,78,69,77,65)[index];
    if (word == 5) return int[3](68,79,70)[index];
    if (word == 6) return int[6](86,79,76,84,65,82)[index];
    return int[6](67,72,85,78,75,83)[index];
}

float drawWord(vec2 uv, vec2 center, float pixelSize, int word) {
    int length = wordLength(word);
    float width = float(length * 6 - 1) * pixelSize;
    vec2 local = (uv - (center - vec2(width * 0.5, 3.5 * pixelSize))) / pixelSize;
    int characterIndex = int(floor(local.x / 6.0));
    int pixelX = int(floor(local.x - float(characterIndex) * 6.0));
    int pixelY = int(floor(local.y));
    if (characterIndex < 0 || characterIndex >= length || pixelX < 0 || pixelX >= 5 || pixelY < 0 || pixelY >= 7) return 0.0;
    int rowMask = glyphRow(wordCharacter(word, characterIndex), 6 - pixelY);
    return float((rowMask >> (4 - pixelX)) & 1);
}

float drawGlyph(vec2 uv, vec2 center, float pixelSize, int glyph) {
    vec2 local = (uv - (center - vec2(2.5, 3.5) * pixelSize)) / pixelSize;
    int pixelX = int(floor(local.x));
    int pixelY = int(floor(local.y));
    if (pixelX < 0 || pixelX >= 5 || pixelY < 0 || pixelY >= 7) return 0.0;
    int rowMask = glyphRow(glyph, 6 - pixelY);
    return float((rowMask >> (4 - pixelX)) & 1);
}

int decimalPower(int exponent) {
    int result = 1;
    for (int i = 0; i < 6; ++i) if (i < exponent) result *= 10;
    return result;
}

float drawIntegerRight(vec2 uv, vec2 rightEdge, float pixelSize, int value) {
    int magnitude = abs(value);
    int digits = magnitude >= 10000 ? 5 : magnitude >= 1000 ? 4 : magnitude >= 100 ? 3 : magnitude >= 10 ? 2 : 1;
    float mask = 0.0;
    for (int i = 0; i < 5; ++i) {
        if (i < digits) {
            int digit = (magnitude / decimalPower(i)) % 10;
            vec2 center = rightEdge - vec2((float(i) * 6.0 + 2.5) * pixelSize, 0.0);
            mask = max(mask, drawDigit(uv, center, pixelSize, digit));
        }
    }
    if (value < 0) {
        vec2 minusCenter = rightEdge - vec2((float(digits) * 6.0 + 2.5) * pixelSize, 0.0);
        mask = max(mask, roundedBox(uv-minusCenter, vec2(2.0*pixelSize,.45*pixelSize),.2*pixelSize));
    }
    return mask;
}

void main() {
    int featureFlags = int(floor(push.ui.w + 0.5));
    bool cinematic = (featureFlags & 1) != 0;
    bool depthOfField = (featureFlags & 2) != 0;
    vec2 texel = 1.0 / push.frame.xy;
    vec2 centered = fragUV - 0.5;
    float radial = dot(centered, centered);
    vec2 aberration = cinematic ? centered * (0.00006 + radial * 0.00016) : vec2(0.0);
    float materialMarker = texture(hdrScene, fragUV).a;
    bool waterSurface = materialMarker > 1.5;
    vec3 color;
    color.r = texture(hdrScene, fragUV + aberration).r;
    color.g = texture(hdrScene, fragUV).g;
    color.b = texture(hdrScene, fragUV - aberration).b;

    // Motion blur somente durante rotação rápida da câmera; movimento normal continua nítido.
    vec2 motion = clamp(push.cameraMotion.xy * 0.34, vec2(-0.018), vec2(0.018));
    float motionStrength = cinematic ? smoothstep(0.0025, 0.018, length(motion)) : 0.0;
    if (motionStrength > 0.0) {
        vec3 motionColor = color;
        for (int i = 1; i <= 5; ++i) {
            float t = (float(i) / 5.0 - 0.5);
            motionColor += texture(hdrScene, clamp(fragUV + motion * t, 0.0, 1.0)).rgb;
        }
        color = mix(color, motionColor / 6.0, motionStrength * 0.52);
    }

    float depth = texture(sceneDepth, fragUV).r;
    float centerDepth = texture(sceneDepth, vec2(0.5)).r;
    float viewDistance = linearViewDepth(depth);
    float focusDistance = linearViewDepth(centerDepth);
    // Profundidade rasa só é ativada quando a mira está realmente encostada em algo.
    float macroFocus = 1.0 - smoothstep(0.90, 1.35, focusDistance);
    float macroCoc = smoothstep(0.45, 2.20, abs(viewDistance - focusDistance)) * macroFocus;
    // Em exploração normal apenas o horizonte extremo recebe uma desfocagem discreta.
    float distantCoc = smoothstep(180.0, 900.0, viewDistance) * (1.0 - macroFocus) * 0.16;
    float coc = max(macroCoc, distantCoc) * step(depth, 0.99999) * (waterSurface ? 0.0 : 1.0);
    vec3 dof = vec3(0.0);
    const vec2 circle[8] = vec2[8](vec2(1,0), vec2(-1,0), vec2(0,1), vec2(0,-1),
        vec2(0.707,0.707), vec2(-0.707,0.707), vec2(0.707,-0.707), vec2(-0.707,-0.707));
    for (int i = 0; i < 8; ++i) dof += texture(hdrScene, fragUV + circle[i] * texel * (1.0 + coc * 4.0)).rgb;
    if (depthOfField) color = mix(color, dof * 0.125, coc * 0.38);

    vec3 bloom = vec3(0.0);
    for (int i = 0; i < 8; ++i) {
        vec3 sampleColor = texture(hdrScene, fragUV + circle[i] * texel * 4.5).rgb;
        bloom += sampleColor * smoothstep(0.82, 2.4, luminance(sampleColor));
    }
    if (cinematic) color += bloom * 0.055;

    float occlusion = 0.0;
    if (depth < 0.9999 && !waterSurface) {
        for (int i = 0; i < 8; ++i) {
            float neighborDepth = texture(sceneDepth, fragUV + circle[i] * texel * 2.5).r;
            occlusion += smoothstep(0.00015, 0.0022, depth - neighborDepth);
        }
        color *= 1.0 - occlusion * 0.022;

        vec2 lightVector = push.sunScreen.xy - fragUV;
        float lightLength = length(lightVector);
        if (lightLength > 0.001 && abs(push.sunScreen.z) > 0.01) {
            vec2 lightStep = lightVector / lightLength * texel * 2.2;
            float contactShadow = 0.0;
            for (int i = 1; i <= 8; ++i) {
                float blocker = texture(sceneDepth, clamp(fragUV + lightStep * float(i), 0.0, 1.0)).r;
                float bias = 0.00012 + float(i) * 0.000035;
                contactShadow += step(blocker + bias, depth) * (1.0 - float(i) / 9.0);
            }
            color *= 1.0 - clamp(contactShadow * 0.075, 0.0, 0.42) * abs(push.sunScreen.z);
        }
    }

    if (cinematic && abs(push.sunScreen.z) > 0.0 && all(greaterThan(push.sunScreen.xy, vec2(-0.15))) && all(lessThan(push.sunScreen.xy, vec2(1.15)))) {
        vec2 delta = (push.sunScreen.xy - fragUV) / 12.0;
        vec2 rayUV = fragUV;
        float rays = 0.0;
        float decay = 1.0;
        for (int i = 0; i < 12; ++i) {
            rayUV += delta;
            vec3 raySample = texture(hdrScene, clamp(rayUV, 0.0, 1.0)).rgb;
            rays += max(luminance(raySample) - 0.75, 0.0) * decay;
            decay *= 0.88;
        }
        vec3 rayColor = push.sunScreen.z < 0.0 ? vec3(0.34, 0.48, 0.82) : vec3(1.0, 0.72, 0.38);
        color += rayColor * rays * 0.018 * abs(push.sunScreen.z);
        vec2 flareAxis = vec2(0.5) - push.sunScreen.xy;
        float sourceVisibility = abs(push.sunScreen.z);
        for (int ghost = 1; ghost <= 4; ++ghost) {
            vec2 ghostPos = push.sunScreen.xy + flareAxis * (0.55 * float(ghost));
            float ghostRadius = 0.018 + float(ghost) * 0.012;
            float ghostMask = exp(-dot(fragUV-ghostPos,fragUV-ghostPos) / (ghostRadius*ghostRadius));
            vec3 ghostColor = ghost % 2 == 0 ? vec3(0.20,0.42,0.75) : vec3(0.85,0.34,0.12);
            color += ghostColor * ghostMask * sourceVisibility * 0.045;
        }
        float flareStreak = exp(-abs(fragUV.y-push.sunScreen.y)*85.0) *
                            exp(-abs(fragUV.x-push.sunScreen.x)*3.5);
        color += rayColor * flareStreak * sourceVisibility * 0.018;
    }

    vec3 north = texture(hdrScene, fragUV + vec2(0, texel.y)).rgb;
    vec3 south = texture(hdrScene, fragUV - vec2(0, texel.y)).rgb;
    vec3 east = texture(hdrScene, fragUV + vec2(texel.x, 0)).rgb;
    vec3 west = texture(hdrScene, fragUV - vec2(texel.x, 0)).rgb;
    if (cinematic) color += (color * 4.0 - north - south - east - west) * 0.026;

    color *= push.sunScreen.w;
    color = aces(max(color, vec3(0.0)));
    color = pow(color, vec3(1.0 / 2.2));
    // Grade cinematográfico analítico (LUT equivalente sem lookup extra).
    vec3 shadows = color * vec3(0.94, 0.98, 1.045);
    vec3 highlights = color * vec3(1.035, 1.012, 0.965);
    color = mix(shadows, highlights, smoothstep(0.18, 0.82, luminance(color)));
    color = mix(vec3(dot(color, vec3(0.333))), color, 1.035);
    if (cinematic) color *= 1.0 - radial * 0.25;
    float grain = hash12(gl_FragCoord.xy + push.frame.z * 91.7) - 0.5;
    if (cinematic) color += grain * 0.012;
    if (push.frame.w > 0.5) color = mix(color * vec3(0.42, 0.78, 0.82), vec3(0.01, 0.16, 0.21), 0.18);

    if (push.ui.z < 0.5) {
        vec2 hudUV = vec2(fragUV.x, 1.0 - fragUV.y);
        float aspect = push.frame.x / push.frame.y;
        vec2 cross = hudUV - vec2(0.5);
        float crosshair = max(roundedBox(cross, vec2(0.0014, 0.012), 0.001),
                              roundedBox(cross, vec2(0.012 / aspect, 0.0014 * aspect), 0.001));
        color = mix(color, vec3(0.95), crosshair * 0.88);

        // HUD inferior inspirado diretamente na referência:
        // moldura medieval em madeira/bronze, slots quadrados, corações e fome compactos,
        // barra de experiência ornamentada e nível central.
        float barY = 0.062 + abs(push.hud.y) * push.hud.z * 0.0012;
        float slotHalfX = 0.0265;
        float slotHalfY = 0.0455;
        float slotStep = 0.0570;
        float barHalfX = slotStep * 4.0 + slotHalfX + 0.0155;
        float barHalfY = 0.0590;

        vec2 barPoint = hudUV - vec2(0.5, barY);

        // Sombra geral.
        float barShadow = roundedBox(barPoint - vec2(0.0, -0.0060),
                                     vec2(barHalfX + 0.0075, barHalfY + 0.0070), 0.0080);
        color = mix(color, vec3(0.010, 0.006, 0.004), barShadow * 0.90);

        // Camadas da moldura: ferro escuro, cobre, filete claro e madeira interna.
        float frameOuter = roundedBox(barPoint, vec2(barHalfX + 0.0060, barHalfY + 0.0035), 0.0075);
        float frameCopper = roundedBox(barPoint, vec2(barHalfX + 0.0022, barHalfY - 0.0010), 0.0055);
        float frameLight = roundedBox(barPoint, vec2(barHalfX - 0.0012, barHalfY - 0.0045), 0.0040);
        float frameWood = roundedBox(barPoint, vec2(barHalfX - 0.0042, barHalfY - 0.0078), 0.0032);
        float frameInset = roundedBox(barPoint, vec2(barHalfX - 0.0080, barHalfY - 0.0110), 0.0022);

        color = mix(color, vec3(0.080, 0.035, 0.022), frameOuter);
        color = mix(color, vec3(0.360, 0.155, 0.082), (frameOuter - frameCopper) * 0.98);
        color = mix(color, vec3(0.690, 0.405, 0.235), (frameCopper - frameLight) * 0.96);
        color = mix(color, vec3(0.875, 0.650, 0.390), (frameLight - frameWood) * 0.82);
        color = mix(color, vec3(0.235, 0.095, 0.050), frameWood * 0.98);
        color = mix(color, vec3(0.055, 0.025, 0.018), frameInset * 0.98);

        // Bordas superior e inferior, como tábuas entalhadas.
        float topBandDark = roundedBox(barPoint - vec2(0.0, barHalfY - 0.0105),
                                       vec2(barHalfX - 0.0100, 0.0048), 0.0018);
        float topBandLight = roundedBox(barPoint - vec2(0.0, barHalfY - 0.0075),
                                        vec2(barHalfX - 0.0140, 0.0013), 0.0007);
        float bottomBand = roundedBox(barPoint + vec2(0.0, barHalfY - 0.0090),
                                      vec2(barHalfX - 0.0110, 0.0040), 0.0015);
        color = mix(color, vec3(0.120, 0.045, 0.026), topBandDark * 0.95);
        color = mix(color, vec3(0.820, 0.535, 0.300), topBandLight * 0.88);
        color = mix(color, vec3(0.120, 0.048, 0.027), bottomBand * 0.92);

        const int blockLayers[9] = int[9](0,2,3,5,6,8,9,10,11);
        for (int i = 0; i < 9; ++i) {
            float x = 0.5 + (float(i) - 4.0) * slotStep;
            vec2 d = hudUV - vec2(x, barY);
            bool selected = i == int(push.hud.x + 0.5);

            // Moldura individual de cada slot.
            float slotOuter = roundedBox(d, vec2(slotHalfX + 0.0022, slotHalfY + 0.0025), 0.0030);
            float slotCopper = roundedBox(d, vec2(slotHalfX - 0.0008, slotHalfY - 0.0010), 0.0021);
            float slotLight = roundedBox(d, vec2(slotHalfX - 0.0030, slotHalfY - 0.0035), 0.0014);
            float slotWell = roundedBox(d, vec2(slotHalfX - 0.0053, slotHalfY - 0.0060), 0.0011);

            vec3 outerColor = selected ? vec3(0.950, 0.730, 0.405) : vec3(0.290, 0.105, 0.057);
            vec3 copperColor = selected ? vec3(0.995, 0.855, 0.560) : vec3(0.570, 0.295, 0.165);
            vec3 lightColor = selected ? vec3(1.000, 0.925, 0.690) : vec3(0.720, 0.485, 0.285);

            color = mix(color, outerColor, slotOuter * 0.99);
            color = mix(color, copperColor, (slotOuter - slotCopper) * 0.98);
            color = mix(color, lightColor, (slotCopper - slotLight) * 0.90);
            color = mix(color, vec3(0.150, 0.060, 0.035), slotLight * 0.98);
            color = mix(color, vec3(0.030, 0.020, 0.017), slotWell * 0.99);

            // Divisória interna vertical de madeira.
            if (i < 8) {
                float divider = roundedBox(hudUV - vec2(x + slotStep * 0.5, barY),
                                           vec2(0.0025, slotHalfY + 0.0010), 0.0010);
                float dividerHi = roundedBox(hudUV - vec2(x + slotStep * 0.5 - 0.0010, barY),
                                             vec2(0.0007, slotHalfY - 0.0030), 0.0004);
                color = mix(color, vec3(0.080, 0.030, 0.020), divider * 0.98);
                color = mix(color, vec3(0.540, 0.280, 0.155), dividerHi * 0.74);
            }

            // Textura do bloco em proporção realmente quadrada na tela.
            float itemHalfY = 0.0305;
            float itemHalfX = itemHalfY / aspect;
            vec2 itemLocal = vec2(d.x / max(itemHalfX * 2.0, 0.0001),
                                  d.y / max(itemHalfY * 2.0, 0.0001)) + 0.5;
            float itemMask = roundedBox(d, vec2(itemHalfX, itemHalfY), 0.0012);
            vec3 itemColor = texture(blockTextures,
                                     vec3(clamp(itemLocal, 0.0, 1.0), float(blockLayers[i]))).rgb;
            color = mix(color, itemColor, itemMask);

            // Rebaixo e brilho no item.
            float itemTop = roundedBox(d - vec2(0.0, itemHalfY - 0.0012),
                                       vec2(itemHalfX - 0.0015, 0.0010), 0.0004);
            float itemBottom = roundedBox(d + vec2(0.0, itemHalfY - 0.0012),
                                          vec2(itemHalfX - 0.0015, 0.0011), 0.0004);
            color = mix(color, vec3(0.840, 0.740, 0.530), itemTop * 0.28);
            color = mix(color, vec3(0.010, 0.008, 0.007), itemBottom * 0.52);

            // Aba clara no topo do slot selecionado.
            if (selected) {
                float selectedCap = roundedBox(hudUV - vec2(x, barY + barHalfY + 0.0015),
                                               vec2(slotHalfX * 0.60, 0.0045), 0.0014);
                float selectedCapHi = roundedBox(hudUV - vec2(x, barY + barHalfY + 0.0040),
                                                 vec2(slotHalfX * 0.46, 0.0012), 0.0005);
                color = mix(color, vec3(0.940, 0.720, 0.430), selectedCap);
                color = mix(color, vec3(1.000, 0.910, 0.665), selectedCapHi);
            }
        }

        // Pequenos encaixes nos quatro cantos da moldura.
        for (int sx = -1; sx <= 1; sx += 2) {
            for (int sy = -1; sy <= 1; sy += 2) {
                vec2 cornerCenter = vec2(0.5 + float(sx) * (barHalfX - 0.009),
                                         barY + float(sy) * (barHalfY - 0.010));
                vec2 cp = (hudUV - cornerCenter) / vec2(0.0080, 0.0110);
                float cornerOuter = diamondShape(cp, vec2(0.72, 0.72));
                float cornerInner = diamondShape(cp, vec2(0.43, 0.43));
                color = mix(color, vec3(0.090, 0.032, 0.020), cornerOuter);
                color = mix(color, vec3(0.760, 0.430, 0.235), cornerInner);
            }
        }

        float statsY = 0.1765;
        float xpY = 0.1430;
        float xpHalfX = barHalfX - 0.0040;

        // Barra de experiência com três molduras.
        float xpShadow = roundedBox(hudUV - vec2(0.5, xpY - 0.0030),
                                    vec2(xpHalfX + 0.0080, 0.0115), 0.0040);
        float xpOuter = roundedBox(hudUV - vec2(0.5, xpY),
                                   vec2(xpHalfX + 0.0050, 0.0090), 0.0032);
        float xpMetal = roundedBox(hudUV - vec2(0.5, xpY),
                                   vec2(xpHalfX + 0.0015, 0.0062), 0.0022);
        float xpBack = roundedBox(hudUV - vec2(0.5, xpY),
                                  vec2(xpHalfX - 0.0015, 0.0038), 0.0014);

        color = mix(color, vec3(0.015, 0.009, 0.006), xpShadow * 0.88);
        color = mix(color, vec3(0.235, 0.090, 0.042), xpOuter * 0.99);
        color = mix(color, vec3(0.735, 0.455, 0.235), (xpOuter - xpMetal) * 0.96);
        color = mix(color, vec3(0.095, 0.060, 0.038), xpMetal * 0.98);
        color = mix(color, vec3(0.012, 0.028, 0.018), xpBack * 0.99);

        // Progresso de XP, aproximadamente como na referência.
        float xpProgress = 0.485;
        float xpInnerHalf = xpHalfX - 0.0040;
        float xpFillHalf = xpInnerHalf * xpProgress;
        float xpFillCenter = 0.5 - xpInnerHalf + xpFillHalf;
        float xpFill = roundedBox(hudUV - vec2(xpFillCenter, xpY),
                                  vec2(xpFillHalf, 0.00265), 0.0011);
        float xpFillHi = roundedBox(hudUV - vec2(xpFillCenter, xpY + 0.00135),
                                    vec2(max(xpFillHalf - 0.0015, 0.0), 0.00065), 0.00035);
        float xpFillLo = roundedBox(hudUV - vec2(xpFillCenter, xpY - 0.00155),
                                    vec2(max(xpFillHalf - 0.0015, 0.0), 0.00055), 0.00030);
        color = mix(color, vec3(0.055, 0.470, 0.115), xpFill);
        color = mix(color, vec3(0.395, 0.930, 0.330), xpFillHi * xpFill);
        color = mix(color, vec3(0.020, 0.185, 0.055), xpFillLo * xpFill);

        // Gemas ao longo da barra e marcador vermelho central.
        for (int i = 0; i < 7; ++i) {
            float gx = 0.5 - xpHalfX * 0.74 + float(i) * (xpHalfX * 1.48 / 6.0);
            vec2 gp = (hudUV - vec2(gx, xpY + 0.0002)) / vec2(0.0048, 0.0058);
            float gemOuter = diamondShape(gp, vec2(0.74, 0.74));
            float gemInner = diamondShape(gp, vec2(0.41, 0.41));
            color = mix(color, vec3(0.080, 0.055, 0.036), gemOuter * 0.95);
            color = mix(color, vec3(0.220, 0.535, 0.440), gemInner * 0.92);
        }
        float centerTick = roundedBox(hudUV - vec2(0.5, xpY - 0.0060),
                                      vec2(0.00125, 0.0040), 0.0005);
        color = mix(color, vec3(0.920, 0.105, 0.145), centerTick);

        // Ornamentos laterais da barra.
        for (int side = -1; side <= 1; side += 2) {
            float s = float(side);
            float edgeX = 0.5 + s * (xpHalfX + 0.0060);

            float stem = roundedBox(hudUV - vec2(edgeX, xpY),
                                    vec2(0.0055, 0.0125), 0.0020);
            float cap = roundedBox(hudUV - vec2(edgeX + s * 0.0110, xpY),
                                   vec2(0.0120, 0.0030), 0.0012);

            vec2 curlP = (hudUV - vec2(edgeX + s * 0.0115, xpY + 0.0100)) /
                         vec2(0.0120, 0.0150);
            curlP.x *= s;
            float curl = ringStroke(curlP - vec2(0.08, 0.00), 0.54, 0.090);
            curl *= step(-0.70, curlP.x) * step(curlP.x, 0.62);

            vec2 lowerP = (hudUV - vec2(edgeX + s * 0.0060, xpY - 0.0100)) /
                          vec2(0.0090, 0.0120);
            lowerP.x *= s;
            float lowerCurl = ringStroke(lowerP - vec2(0.05, 0.02), 0.48, 0.095);
            lowerCurl *= step(-0.58, lowerP.x) * step(lowerP.x, 0.56);

            float ornament = max(max(stem, cap), max(curl, lowerCurl));
            color = mix(color, vec3(0.105, 0.045, 0.025), ornament * 0.99);

            float ornamentHi = roundedBox(hudUV - vec2(edgeX - s * 0.0013, xpY + 0.0010),
                                          vec2(0.0010, 0.0090), 0.00045);
            color = mix(color, vec3(0.735, 0.455, 0.245), ornamentHi * ornament * 0.82);
        }

        // Dez corações compactos à esquerda.
        float statStep = 0.0238;
        float heartStart = 0.5 - xpHalfX + 0.0175;
        for (int i = 0; i < 10; ++i) {
            vec2 hp = (hudUV - vec2(heartStart + float(i) * statStep, statsY)) /
                      vec2(0.0124, 0.0172);
            float heartOuter = heartShape(hp * 0.80);
            float heartMetal = heartShape(hp * 0.89);
            float heart = heartShape(hp);
            float heartInner = heartShape(hp * 1.20);

            color = mix(color, vec3(0.080, 0.010, 0.018), heartOuter);
            color = mix(color, vec3(0.720, 0.410, 0.375), (heartOuter - heartMetal) * 0.82);
            color = mix(color, vec3(0.950, 0.045, 0.115), heart);
            color = mix(color, vec3(1.000, 0.255, 0.355), heartInner * 0.68);

            float shine = 1.0 - smoothstep(0.13, 0.24, length(hp - vec2(-0.26, 0.31)));
            float lowerShade = (1.0 - smoothstep(-0.60, -0.10, hp.y)) * heart;
            color = mix(color, vec3(1.000, 0.870, 0.900), shine * heart);
            color = mix(color, vec3(0.500, 0.012, 0.055), lowerShade * 0.33);
        }

        // Dez ícones de fome (coxa/carne com osso), à direita.
        float foodRight = 0.5 + xpHalfX - 0.0165;
        for (int i = 0; i < 10; ++i) {
            vec2 fp = (hudUV - vec2(foodRight - float(i) * statStep, statsY)) /
                      vec2(0.0134, 0.0170);

            float foodOuter = foodShape(fp * 0.79);
            float food = foodShape(fp);
            float bone = foodBone(fp);
            float meat = max(food - bone, 0.0);

            color = mix(color, vec3(0.075, 0.040, 0.020), foodOuter);
            color = mix(color, vec3(0.515, 0.265, 0.085), food);
            color = mix(color, vec3(0.220, 0.105, 0.035), meat * 0.88);
            color = mix(color, vec3(0.885, 0.555, 0.205), bone);

            float meatShine = 1.0 - smoothstep(0.11, 0.21, length(fp - vec2(-0.28, 0.28)));
            float boneShine = 1.0 - smoothstep(0.10, 0.18, length(fp - vec2(0.46, -0.02)));
            color = mix(color, vec3(0.760, 0.430, 0.155), meatShine * meat * 0.60);
            color = mix(color, vec3(1.000, 0.760, 0.345), boneShine * bone * 0.55);
        }

        // Nível central com sombra grossa e preenchimento marfim/dourado.
        float digitSize = 0.00425;
        float levelOutline = 0.0;
        const vec2 outlineOffsets[8] = vec2[8](
            vec2(-1.0, 0.0), vec2(1.0, 0.0), vec2(0.0, -1.0), vec2(0.0, 1.0),
            vec2(-0.72, -0.72), vec2(0.72, -0.72), vec2(-0.72, 0.72), vec2(0.72, 0.72)
        );
        for (int oi = 0; oi < 8; ++oi) {
            vec2 off = outlineOffsets[oi] * digitSize * 0.55;
            levelOutline = max(levelOutline,
                max(drawDigit(hudUV, vec2(0.4910, statsY) + off, digitSize, 3),
                    drawDigit(hudUV, vec2(0.5110, statsY) + off, digitSize, 5)));
        }
        float levelText = max(drawDigit(hudUV, vec2(0.4910, statsY), digitSize, 3),
                              drawDigit(hudUV, vec2(0.5110, statsY), digitSize, 5));
        color = mix(color, vec3(0.040, 0.022, 0.012), levelOutline);
        color = mix(color, vec3(0.930, 0.825, 0.555), levelText);

        vec2 mapCenter=vec2(.905,.855); vec2 mp=(hudUV-mapCenter)*vec2(aspect,1);
        float mapRadius=.102; float mapCircle=1.0-smoothstep(mapRadius-.002,mapRadius,length(mp));
        vec2 mapUV=mp/(mapRadius*2.0)+.5;
        vec3 mapColor=texture(minimapScene,clamp(mapUV,0.0,1.0)).rgb;
        color=mix(color,mapColor,mapCircle);
        float mapRim=(1.0-smoothstep(mapRadius,mapRadius+.010,length(mp)))-
                     (1.0-smoothstep(mapRadius-.010,mapRadius,length(mp)));
        color=mix(color,vec3(.24,.25,.24),mapRim);
        float innerRim=(1.0-smoothstep(mapRadius-.008,mapRadius-.004,length(mp)))-
                       (1.0-smoothstep(mapRadius-.014,mapRadius-.010,length(mp)));
        color=mix(color,vec3(.82,.79,.68),innerRim);
        vec2 arrow=mp/vec2(.012,.018); float playerArrow=step(abs(arrow.x),max(0.0,1.0-arrow.y)) * step(-.65,arrow.y)*step(arrow.y,1.0);
        color=mix(color,vec3(.95,.22,.10),playerArrow);
        float north=roundedBox(hudUV-vec2(mapCenter.x,mapCenter.y+mapRadius+.016),vec2(.004,.010),.001);
        color=mix(color,vec3(.92,.80,.55),north);

        // Coordenadas reais sob o minimapa.
        vec2 coordCenter=vec2(mapCenter.x,mapCenter.y-mapRadius-.025);
        float coordShadow=roundedBox(hudUV-(coordCenter-vec2(0,.003)),vec2(.087,.020),.006);
        float coordPanel=roundedBox(hudUV-coordCenter,vec2(.084,.017),.005);
        color=mix(color,vec3(.015,.018,.018),coordShadow*.72);
        color=mix(color,vec3(.055,.065,.062),coordPanel*.92);
        float coordTop=roundedBox(hudUV-(coordCenter+vec2(0,.0145)),vec2(.078,.001),.0005);
        color=mix(color,vec3(.63,.57,.43),coordTop);
        float coordPixel=.00155;
        float coordGlyph=max(drawGlyph(hudUV,coordCenter+vec2(-.073,0),coordPixel,88),
                         max(drawGlyph(hudUV,coordCenter+vec2(-.018,0),coordPixel,89),
                             drawGlyph(hudUV,coordCenter+vec2(.037,0),coordPixel,90)));
        float coordDigits=max(drawIntegerRight(hudUV,coordCenter+vec2(-.026,0),coordPixel,int(round(push.cameraMotion.z))),
                         max(drawIntegerRight(hudUV,coordCenter+vec2(.029,0),coordPixel,int(round(push.hud.w))),
                             drawIntegerRight(hudUV,coordCenter+vec2(.082,0),coordPixel,int(round(push.cameraMotion.w)))));
        color=mix(color,vec3(.88,.88,.80),max(coordGlyph,coordDigits));
    }

    if (push.ui.z > 0.5) {
        vec2 menuUV = vec2(fragUV.x, 1.0 - fragUV.y);
        vec3 blurred = vec3(0.0);
        for (int i = 0; i < 8; ++i) blurred += texture(hdrScene, fragUV + circle[i] * texel * 10.0).rgb;
        blurred = pow(aces(blurred * 0.125 * push.sunScreen.w), vec3(1.0 / 2.2));
        color = mix(color, blurred, 0.34) * 0.30;

        float panel = roundedBox(menuUV - vec2(0.5), vec2(0.235, 0.305), 0.025);
        color = mix(color, vec3(0.025, 0.032, 0.045), panel * 0.94);
        float panelEdge = roundedBox(menuUV - vec2(0.5), vec2(0.238, 0.308), 0.027) - panel;
        color = mix(color, vec3(0.18, 0.48, 0.72), clamp(panelEdge * 1.7, 0.0, 1.0));

        bool graphicsPage = push.ui.z > 1.5;
        int titleWord = graphicsPage ? 2 : 0;
        float title = drawWord(menuUV, vec2(0.5, 0.705), 0.0045, titleWord);
        color = mix(color, vec3(0.78, 0.91, 1.0), title);

        const float graphicsButtonCenters[4] = float[4](0.575, 0.475, 0.375, 0.275);
        const float pauseButtonCenters[3] = float[3](0.56, 0.44, 0.32);
        int buttonCount = graphicsPage ? 4 : 3;
        for (int button = 0; button < 4; ++button) {
            if (button >= buttonCount) continue;
            float centerY = graphicsPage ? graphicsButtonCenters[button] : pauseButtonCenters[button];
            vec2 centerPoint = vec2(0.5, centerY);
            float buttonMask = roundedBox(menuUV - centerPoint, vec2(0.16, 0.045), 0.012);
            bool hover = abs(push.ui.x - 0.5) <= 0.16 && abs(push.ui.y - centerY) <= 0.045;
            bool enabled = graphicsPage && ((button == 0 && cinematic) || (button == 1 && depthOfField));
            vec3 buttonColor = enabled ? vec3(0.07, 0.34, 0.30) : vec3(0.075, 0.095, 0.125);
            if (hover) buttonColor = enabled ? vec3(0.09, 0.52, 0.42) : vec3(0.12, 0.28, 0.43);
            color = mix(color, buttonColor, buttonMask);
            int label = graphicsPage ? (button == 0 ? 4 : (button == 1 ? 5 : (button == 2 ? 7 : 6)))
                                     : (button == 0 ? 1 : (button == 1 ? 2 : 3));
            vec2 labelCenter = graphicsPage && button == 2 ? centerPoint - vec2(0.035, 0.0) : centerPoint;
            float text = drawWord(menuUV, labelCenter, 0.0027, label);
            if (graphicsPage && button == 2) {
                text = max(text, drawIntegerRight(menuUV, centerPoint + vec2(0.130, 0.0), 0.0027,
                                                   int(round(push.settings.x))));
                float minusMark = roundedBox(menuUV - (centerPoint - vec2(0.138, 0.0)),
                                             vec2(0.008, 0.0012), 0.0008);
                vec2 plusCenter = centerPoint + vec2(0.138, 0.0);
                float plusMark = max(roundedBox(menuUV - plusCenter, vec2(0.008, 0.0012), 0.0008),
                                     roundedBox(menuUV - plusCenter, vec2(0.0012, 0.010), 0.0008));
                color = mix(color, hover ? vec3(1.0) : vec3(0.80, 0.87, 0.92),
                            max(minusMark, plusMark));
            }
            color = mix(color, hover ? vec3(1.0) : vec3(0.80, 0.87, 0.92), text);
        }
    }
    outColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
