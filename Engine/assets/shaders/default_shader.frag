#version 450

// ============================================================
// Default Material Shader - Vulkan Fragment Shader
// PCF shadows + hemisphere ambient + per-vertex AO
// ============================================================

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragWorldNorm;
layout(location = 2) in vec3 fragWorldTan;
layout(location = 3) in vec3 fragWorldBit;
layout(location = 4) in vec4 fragColor;
layout(location = 5) in vec2 fragUV;
layout(location = 6) in vec4 fragShadowCoord;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D diffuseTexture;

layout(set = 2, binding = 0) uniform ShadowLightUBO {
    vec4  uLightDir;
    vec4  uLightColor;
    vec4  uAmbient;        // rgb = sky ambient color, a = intensity
    vec4  uCameraPos;
    mat4  uLightVP;
    float uShadowBias;
    float uShadowStrength;
    float uShadowTexelSize;
    float uLightSize;
    vec4  uFogColor;       // horizon
    float uFogStart;
    float uFogEnd;
    vec2  _fogPad;         // std140: must be vec2 (8B), NOT float[2] (stride-16 = 32B)
    vec4  uFogZenithColor; // zenith
    float uHeightFogTop;
    float uHeightFogDensity;
    float uHeightFogFalloff;
    float uHeightFogEnabled;
};

layout(set = 3, binding = 0) uniform sampler2DShadow shadowMap;     // hardware PCF
layout(set = 3, binding = 1) uniform sampler2D       shadowMapRaw;  // blocker search

float InterleavedGradientNoise(vec2 screenPos) {
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(screenPos, magic.xy)));
}

// Vogel disk — quasi-uniform distribution without banding, works at any
// sample count. Each sample rotated by a per-pixel golden-angle offset.
vec2 VogelDisk(int i, int count, float phi) {
    const float goldenAngle = 2.39996323;
    float r = sqrt((float(i) + 0.5) / float(count));
    float theta = float(i) * goldenAngle + phi;
    return vec2(r * cos(theta), r * sin(theta));
}

const int BLOCKER_SAMPLES = 16;
const int PCF_SAMPLES     = 48;

float FindAvgBlockerDepth(vec2 uv, float zRef, float searchRadius, float phi) {
    float sum = 0.0;
    int count = 0;
    for (int i = 0; i < BLOCKER_SAMPLES; ++i) {
        vec2 off = VogelDisk(i, BLOCKER_SAMPLES, phi) * searchRadius;
        float d = texture(shadowMapRaw, uv + off).r;
        if (d < zRef) {
            sum += d;
            count++;
        }
    }
    return (count > 0) ? sum / float(count) : -1.0;
}

float PCFFilter(vec2 uv, float zRef, float filterRadius, float phi) {
    float shadow = 0.0;
    for (int i = 0; i < PCF_SAMPLES; ++i) {
        vec2 off = VogelDisk(i, PCF_SAMPLES, phi) * filterRadius;
        shadow += texture(shadowMap, vec3(uv + off, zRef));
    }
    return shadow / float(PCF_SAMPLES);
}

// PCSS for directional (orthographic) light:
//   - Fixed blocker search window (30 texels) decoupled from uLightSize so
//     the blocker detection stays reliable at all softness settings.
//   - Penumbra is linear in the blocker-receiver depth gap; no perspective
//     divide (that formula only applies to perspective/point lights).
//   - uLightSize acts as a softness multiplier — larger → wider penumbra.
float CalcShadow(vec4 sc) {
    vec3 projCoords = sc.xyz / sc.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z < 0.0 || projCoords.z > 1.0)
        return 1.0;

    vec2 fadeCoord = smoothstep(vec2(0.0), vec2(0.05), projCoords.xy)
                   * smoothstep(vec2(0.0), vec2(0.05), vec2(1.0) - projCoords.xy);
    float zFade    = smoothstep(0.0, 0.05, projCoords.z)
                   * smoothstep(0.0, 0.1,  1.0 - projCoords.z);
    float edgeFade = fadeCoord.x * fadeCoord.y * zFade;

    float zRef = projCoords.z - uShadowBias;
    float phi  = InterleavedGradientNoise(gl_FragCoord.xy) * 6.283185;

    // Fixed 30-texel blocker search: detects blockers ~0.7m above receiver
    // on a 28m/4096 map. Decoupled from uLightSize for stability.
    float searchRadius = uShadowTexelSize * 30.0;
    float avgBlocker   = FindAvgBlockerDepth(projCoords.xy, zRef, searchRadius, phi);

    if (avgBlocker < 0.0) return 1.0;

    // Linear penumbra for orthographic projection:
    // scale=500 calibrated for a ~28m frustum / 4096-texel map.
    float gap          = max(zRef - avgBlocker, 0.0);
    float filterRadius = clamp(gap * uLightSize * uShadowTexelSize * 500.0,
                               uShadowTexelSize * 0.5,   // floor: contact-hard
                               uShadowTexelSize * 20.0); // cap: floating objects

    float shadow = PCFFilter(projCoords.xy, zRef, filterRadius, phi);
    return mix(1.0, shadow, uShadowStrength * edgeFade);
}

// ACES film tone mapping (Stephen Hill fit)
vec3 ACESFilm(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), vec3(0.0), vec3(1.0));
}

// Sky-matched gradient fog + exponential height fog. View direction picks a
// blend between horizon and zenith colors so distant terrain dissolves into
// the same color the sky renders behind it.
vec3 ApplyFog(vec3 color, vec3 worldPos) {
    if (uFogEnd <= 0.0) return color;
    vec3 toFrag = worldPos - uCameraPos.xyz;
    float dist = length(toFrag.xz);
    float distFog = clamp((dist - uFogStart) / max(uFogEnd - uFogStart, 1e-4), 0.0, 1.0);
    float heightFog = 0.0;
    if (uHeightFogEnabled > 0.5) {
        float h = max(uHeightFogTop - worldPos.y, 0.0);
        heightFog = clamp(uHeightFogDensity * (1.0 - exp(-h * uHeightFogFalloff)),
                          0.0, 1.0);
    }
    float fogAmount = 1.0 - (1.0 - distFog) * (1.0 - heightFog);
    vec3 viewDir = normalize(toFrag);
    float t = smoothstep(0.0, 1.0, clamp(viewDir.y * 0.5 + 0.5, 0.0, 1.0));
    vec3 fogColor = mix(uFogColor.rgb, uFogZenithColor.rgb, t);
    return mix(color, fogColor, fogAmount);
}

void main() {
    vec4 texColor = texture(diffuseTexture, fragUV);
    if (texColor.a < 0.5)
        discard;

    // AO is stored in vertex color (r channel). Apply only to ambient.
    float ao       = fragColor.r;
    vec3 baseColor = texColor.rgb;

    vec3 N            = normalize(fragWorldNorm);
    vec3 lightDir     = normalize(uLightDir.xyz);
    vec3 lightColor   = uLightColor.rgb;
    float lightIntens = uLightColor.a;

    // Hemisphere ambient: sky color above, ground-bounce below
    vec3 skyAmbient    = uAmbient.rgb * uAmbient.a;
    vec3 groundAmbient = skyAmbient * vec3(0.55, 0.50, 0.45);
    float hemisphere   = N.y * 0.5 + 0.5;
    vec3 ambient       = mix(groundAmbient, skyAmbient, hemisphere);

    // Wrap diffuse: softens shadow terminator, side faces get ~15% sun
    float NdotL   = clamp(dot(N, -lightDir) * 0.85 + 0.15, 0.0, 1.0);
    float shadow  = CalcShadow(fragShadowCoord);
    vec3  diffuse = lightColor * lightIntens * NdotL * shadow;

    // Compose: AO only on ambient (direct light ignores occlusion)
    vec3 lit = baseColor * (ao * ambient + diffuse);

    // ACES tone mapping
    lit = ACESFilm(lit);

    lit = ApplyFog(lit, fragWorldPos);

    outColor = vec4(lit, texColor.a);
}
