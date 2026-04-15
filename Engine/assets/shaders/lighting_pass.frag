#version 450

// ============================================================
// Deferred Lighting Pass - Vulkan Fragment Shader
// Samples world position directly from GBuffer RT3, applies
// diffuse shading with PCF shadow mapping.
//
// Set 0: GBuffer samplers (binding 0-4) + shadow map (binding 5)
// Set 1: DeferredCB UBO (InvViewProj + screen size + near/far)
// Set 2: ShadowLightUBO (directional light parameters)
// ============================================================

layout(location = 0) in vec2 fragUV;

// ----------------------------------------------------------
// Set 0: GBuffer textures
// ----------------------------------------------------------
layout(set = 0, binding = 0) uniform sampler2D gAlbedoAO;     // Albedo.rgb + AO.a
layout(set = 0, binding = 1) uniform sampler2D gNormalRough;  // Normal.xyz (encoded) + Roughness.a
layout(set = 0, binding = 2) uniform sampler2D gMetalEmit;    // Metallic.r + EmissiveScale.g
layout(set = 0, binding = 3) uniform sampler2D gWorldPos;     // World position (RT3)
layout(set = 0, binding = 4) uniform sampler2D gDepth;        // Depth buffer
layout(set = 0, binding = 5) uniform sampler2DShadow gShadow; // Shadow map

// ----------------------------------------------------------
// Set 1: Deferred constant buffer
// ----------------------------------------------------------
layout(set = 1, binding = 0) uniform DeferredCB {
    mat4  InvViewProj;
    float ScreenW;
    float ScreenH;
    float NearP;
    float FarP;
};

// ----------------------------------------------------------
// Set 2: Light/shadow UBO (matches ShadowLightUBO in vertex shaders)
// ----------------------------------------------------------
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
    vec4  uFogColor;          // horizon color
    float uFogStart;
    float uFogEnd;
    vec2  _fogPad;            // std140: must be vec2 (8B), NOT float[2] (stride-16 = 32B)
    vec4  uFogZenithColor;    // zenith color
    float uHeightFogTop;
    float uHeightFogDensity;
    float uHeightFogFalloff;
    float uHeightFogEnabled;
};

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

layout(location = 0) out vec4 outColor;

// ----------------------------------------------------------
// Helpers
// ----------------------------------------------------------

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

float CalcShadow(vec3 worldPos, vec3 N) {
    // Normal bias: offset along surface normal to reduce shadow acne
    // (matches forward shader's vertex-level normal bias in uLightDir.w)
    float normalBias = uLightDir.w;
    vec3 biasedPos = worldPos + N * normalBias;
    vec4 sc = uLightVP * vec4(biasedPos, 1.0);
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

    float biasedDepth = projCoords.z - uShadowBias;
    float texelSize   = uShadowTexelSize;
    float radius      = 1.5;
    float shadow      = 0.0;

    for (int i = 0; i < 16; i++) {
        shadow += texture(gShadow,
            vec3(projCoords.xy + poissonDisk[i] * texelSize * radius,
                 biasedDepth));
    }
    shadow /= 16.0;
    shadow  = mix(1.0, shadow, uShadowStrength * edgeFade);
    return shadow;
}

// ACES film tone mapping
vec3 ACESFilm(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), vec3(0.0), vec3(1.0));
}

void main() {
    // Sample depth — early discard for sky pixels
    float depth = texture(gDepth, fragUV).r;
    if (depth >= 1.0) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // World position read DIRECTLY from GBuffer RT3 — never reconstructed
    // via InvViewProj * ndcPos. Reconstruction depends on the camera
    // view-projection matrix, so under pure rotation it produces sub-pixel
    // FP drift in worldPos that propagates into shadow-map sample coords
    // and shows up as visible "shadow shimmer" on every rotation tick.
    vec3 worldPos = texture(gWorldPos, fragUV).xyz;

    // Sample GBuffer
    vec4 albedoAO     = texture(gAlbedoAO,    fragUV);
    vec4 normalRough  = texture(gNormalRough,  fragUV);
    vec4 metalEmit    = texture(gMetalEmit,    fragUV);

    vec3 albedo    = albedoAO.rgb;
    float ao       = albedoAO.a;
    vec3 N         = normalize(normalRough.rgb * 2.0 - 1.0);
    float roughness = normalRough.a;
    float metallic  = metalEmit.r;

    // Light direction (uLightDir points FROM light, negate for L)
    vec3 lightDir = normalize(uLightDir.xyz);
    vec3 L        = -lightDir;

    // Hemisphere ambient (matches forward shader)
    vec3 skyAmbient    = uAmbient.rgb * uAmbient.a;
    vec3 groundAmbient = skyAmbient * vec3(0.55, 0.50, 0.45);
    float hemisphere   = N.y * 0.5 + 0.5;
    vec3 ambient       = mix(groundAmbient, skyAmbient, hemisphere);

    // Wrap diffuse (matches forward shader 0.85 wrap)
    float rawNdotL = dot(N, L);
    float NdotL    = clamp(rawNdotL * 0.85 + 0.15, 0.0, 1.0);

    // Shadow
    float shadow = CalcShadow(worldPos, N);
    // Fade shadow for back-facing surfaces (prevents light leak in caves)
    shadow *= smoothstep(0.0, 0.15, rawNdotL);

    vec3 lightColor = uLightColor.rgb * uLightColor.a;
    vec3 diffuse    = lightColor * NdotL * shadow;

    // Compose: AO only on ambient (matches forward shader exactly)
    vec3 lit = albedo * (ao * ambient + diffuse);

    // ACES tone mapping
    lit = ACESFilm(lit);

    lit = ApplyFog(lit, worldPos);

    outColor = vec4(lit, 1.0);
}
