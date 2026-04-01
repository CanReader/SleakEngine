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
    vec4  uFogColor;
    float uFogStart;
    float uFogEnd;
    float _fogPad[2];
};

layout(set = 3, binding = 0) uniform sampler2DShadow shadowMap;

// 16-sample Poisson disk for PCF
const vec2 poissonDisk[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870), vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845), vec2( 0.97484398,  0.75648379),
    vec2( 0.44323325, -0.97511554), vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507), vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367), vec2( 0.14383161, -0.14100790)
);

float CalcShadow(vec4 sc) {
    vec3 projCoords = sc.xyz / sc.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    // Out-of-bounds check
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z < 0.0 || projCoords.z > 1.0)
        return 1.0;

    // Smooth fade at shadow map edges (XY) and far depth (Z) to avoid hard cutoffs
    vec2 fadeCoord = smoothstep(vec2(0.0), vec2(0.05), projCoords.xy)
                   * smoothstep(vec2(0.0), vec2(0.05), vec2(1.0) - projCoords.xy);
    float zFade    = smoothstep(0.0, 0.05, projCoords.z)
                   * smoothstep(0.0, 0.1,  1.0 - projCoords.z);
    float edgeFade = fadeCoord.x * fadeCoord.y * zFade;

    float biasedDepth = projCoords.z - uShadowBias;

    // 16-sample Poisson PCF
    float texelSize = uShadowTexelSize;
    float radius = 1.5;
    float shadow = 0.0;

    for (int i = 0; i < 16; i++) {
        shadow += texture(shadowMap,
            vec3(projCoords.xy + poissonDisk[i] * texelSize * radius,
                 biasedDepth));
    }
    shadow /= 16.0;

    // Apply edge fade and shadow strength
    shadow = mix(1.0, shadow, uShadowStrength * edgeFade);
    return shadow;
}

// ACES film tone mapping (Stephen Hill fit)
vec3 ACESFilm(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), vec3(0.0), vec3(1.0));
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

    // Distance fog (horizontal XZ only — altitude doesn't affect fog)
    if (uFogEnd > 0.0) {
        float dist = length(fragWorldPos.xz - uCameraPos.xz);
        float fogFactor = clamp((uFogEnd - dist) / (uFogEnd - uFogStart), 0.0, 1.0);
        lit = mix(uFogColor.rgb, lit, fogFactor);
    }

    outColor = vec4(lit, texColor.a);
}
