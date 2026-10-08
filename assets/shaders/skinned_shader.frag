#version 450

// ============================================================
// Skinned Material Shader - Vulkan Fragment Shader
// PCSS soft shadows with contact hardening + hemisphere ambient
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
    vec4  uAmbient;
    vec4  uCameraPos;
    mat4  uLightVP;
    float uShadowBias;
    float uShadowStrength;
    float uShadowTexelSize;
    float uLightSize;
    vec4  uFogColor;
    float uFogStart;
    float uFogEnd;
    vec2  _fogPad;
    vec4  uFogZenithColor;
    float uHeightFogTop;
    float uHeightFogDensity;
    float uHeightFogFalloff;
    float uHeightFogEnabled;
    vec4  uExtraDir[3];
    vec4  uExtraColor[3];
    uint  uNumExtraLights;
    mat4  uNdcToShadow;
    mat4  uCascadeVP[4];
    vec4  uCascadeSplits;    // cascade radius around uCameraPos
    uint  uCascadeCount;
    float uCascadeBlend;     // blend band, fraction of the radius
};

layout(set = 3, binding = 2) uniform sampler2DArrayShadow shadowMap;  // cascades, PCF
layout(set = 3, binding = 3) uniform sampler2DArray       shadowMapRaw;  // blocker search

// ------------------------------------------------------------
// Sampling helpers
// ------------------------------------------------------------

float InterleavedGradientNoise(vec2 screenPos) {
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(screenPos, magic.xy)));
}

// Vogel disk — quasi-uniform low-discrepancy sample distribution.
// Beats a hand-authored Poisson table: works at any sample count and has
// no precomputed clumps. Combined with per-pixel rotation it eliminates
// the banding that shows up on large PCF kernels.
vec2 VogelDisk(int i, int count, float phi) {
    const float goldenAngle = 2.39996323;
    float r = sqrt((float(i) + 0.5) / float(count));
    float theta = float(i) * goldenAngle + phi;
    return vec2(r * cos(theta), r * sin(theta));
}

// ------------------------------------------------------------
// PCSS: Percentage-Closer Soft Shadows
// ------------------------------------------------------------
// Stage 1 (blocker search):  average depth of occluders inside a search disc.
// Stage 2 (penumbra size):   (receiver - blocker) / blocker * lightSize.
//                            → shadows contact-harden: crisp where the
//                              caster touches the receiver, soft far away.
// Stage 3 (PCF):             hardware-compared samples over the penumbra disc.

const int BLOCKER_SAMPLES = 16;
const int PCF_SAMPLES     = 48;

float FindAvgBlockerDepth(vec2 uv, float layer, float zRef,
                          float searchRadius, float phi) {
    float sum = 0.0;
    int count = 0;
    for (int i = 0; i < BLOCKER_SAMPLES; ++i) {
        vec2 off = VogelDisk(i, BLOCKER_SAMPLES, phi) * searchRadius;
        float d = texture(shadowMapRaw, vec3(uv + off, layer)).r;
        if (d < zRef) {
            sum += d;
            count++;
        }
    }
    return (count > 0) ? sum / float(count) : -1.0;
}

float PCFFilter(vec2 uv, float layer, float zRef, float filterRadius,
                float phi) {
    float shadow = 0.0;
    for (int i = 0; i < PCF_SAMPLES; ++i) {
        vec2 off = VogelDisk(i, PCF_SAMPLES, phi) * filterRadius;
        shadow += texture(shadowMap, vec4(uv + off, layer, zRef));
    }
    return shadow / float(PCF_SAMPLES);
}

// PCSS for directional (orthographic) light:
//   - Blocker search uses a fixed-size window large enough to capture
//     blockers at typical caster heights (independent of uLightSize so
//     the search doesn't shrink when the user wants crisp shadows).
//   - Penumbra is linear in the blocker-receiver depth gap; no perspective
//     divide (that formula only applies to perspective/point lights).
//   - uLightSize acts as a softness multiplier — larger → wider penumbra.
float CalcShadow(vec4 sc, float layer) {
    vec3 projCoords = sc.xyz / sc.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z < 0.0 || projCoords.z > 1.0)
        return 1.0;

    float edgeFade = smoothstep(0.0, 0.05, projCoords.z)
                   * smoothstep(0.0, 0.08, 1.0 - projCoords.z);

    float zRef = projCoords.z - uShadowBias;
    float phi  = InterleavedGradientNoise(gl_FragCoord.xy) * 6.283185;

    // Fixed search window: 30 texels radius, detects blockers up to ~0.7m
    // above receiver on a 28m/4096 map — decoupled from uLightSize so the
    // blocker search stays reliable at all softness settings.
    float searchRadius = uShadowTexelSize * 30.0;
    float avgBlocker   = FindAvgBlockerDepth(projCoords.xy, layer, zRef,
                                             searchRadius, phi);

    if (avgBlocker < 0.0) return 1.0;

    // Linear penumbra for orthographic projection:
    // gap (NDC depth diff) * uLightSize * scale → filter radius in UV space.
    // scale=500 calibrated for a ~28m frustum / 4096-texel map so that
    // uLightSize=1 gives natural softness; raise uLightSize for wider penumbra.
    float gap          = max(zRef - avgBlocker, 0.0);
    float filterRadius = clamp(gap * uLightSize * uShadowTexelSize * 500.0,
                               uShadowTexelSize * 0.5,   // floor: contact-hard
                               uShadowTexelSize * 20.0); // cap: floating objects

    float shadow = PCFFilter(projCoords.xy, layer, zRef, filterRadius, phi);
    return mix(1.0, shadow, uShadowStrength * edgeFade);
}

// Normal offset scales with the cascade radius so it stays the same number
// of texels in every cascade.
vec4 CascadeCoord(uint c, vec3 worldPos, vec3 N) {
    float normalBias = uLightDir.w * uCascadeSplits[c] /
                       uCascadeSplits[uCascadeCount - 1u];
    return uCascadeVP[c] * vec4(worldPos + N * normalBias, 1.0);
}

// Picks the cascade by distance from the camera and blends into the next one
// (or out to unshadowed after the last) across the band at its outer edge.
float CalcCascadedShadow(vec3 worldPos, vec3 N) {
    if (uCascadeCount == 0u) return 1.0;
    float dist = length(worldPos - uCameraPos.xyz);
    uint  c    = 0u;
    while (c + 1u < uCascadeCount && dist > uCascadeSplits[c]) ++c;

    float outer = uCascadeSplits[c];
    if (dist >= outer) return 1.0;
    float t      = smoothstep(outer * (1.0 - uCascadeBlend), outer, dist);
    float shadow = CalcShadow(CascadeCoord(c, worldPos, N), float(c));
    if (t > 0.0) {
        float next = (c + 1u < uCascadeCount)
            ? CalcShadow(CascadeCoord(c + 1u, worldPos, N), float(c + 1u))
            : 1.0;
        shadow = mix(shadow, next, t);
    }
    return shadow;
}

void main() {
    vec4 texColor = texture(diffuseTexture, fragUV);
    vec4 baseColor = texColor * fragColor;

    vec3 N = normalize(fragWorldNorm);
    vec3 lightDir = normalize(uLightDir.xyz);
    vec3 viewDir = normalize(uCameraPos.xyz - fragWorldPos);
    vec3 lightColor = uLightColor.rgb;
    float lightIntensity = uLightColor.a;

    vec3 ambientColor = uAmbient.rgb * uAmbient.a;
    vec3 groundColor = ambientColor * vec3(0.7, 0.65, 0.6);
    float hemisphere = N.y * 0.5 + 0.5;
    vec3 ambient = mix(groundColor, ambientColor, hemisphere);

    float NdotL = dot(N, -lightDir);
    float diffuseTerm = max(NdotL, 0.0);
    vec3 diffuse = lightColor * lightIntensity * diffuseTerm;

    vec3 halfDir = normalize(-lightDir + viewDir);
    float shininess = 32.0;
    float normFactor = (shininess + 8.0) / 25.1327;
    float spec = normFactor * pow(max(dot(N, halfDir), 0.0), shininess);
    vec3 specular = lightColor * spec * 0.5 * max(NdotL, 0.0);

    float shadow = CalcCascadedShadow(fragWorldPos, N);

    // Output linear HDR — tonemapping (ACES + gamma) is applied in the
    // bloom composite pass. Doing Reinhard here would cause double-tonemapping.
    vec3 lit = baseColor.rgb * (ambient + shadow * diffuse)
             + shadow * specular;

    outColor = vec4(lit, baseColor.a);
}
