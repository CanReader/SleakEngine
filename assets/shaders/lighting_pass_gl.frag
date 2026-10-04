#version 450 core

// Deferred lighting pass (OpenGL).
// Reads the GBuffer (units 8-11), reconstructs world position from depth,
// lights it with the main directional light (cascaded shadows), any extra
// directional lights, and the local lights of the pixel's cluster.

in  vec2 fragUV;
out vec4 outColor;

layout(binding = 8)  uniform sampler2D gbAlbedoAO;
layout(binding = 9)  uniform sampler2D gbNormalRough;
layout(binding = 10) uniform sampler2D gbMetalEmit;
layout(binding = 11) uniform sampler2D gbDepth;

// Blurred R8 SSAO, 1.0 means fully lit
layout(binding = 13) uniform sampler2D gbSSAO;
uniform int uSSAOEnabled;

layout(binding = 14) uniform samplerCube irradianceMap;
layout(binding = 15) uniform samplerCube prefilterMap;
layout(binding = 16) uniform sampler2D   brdfLUT;

// Shadow cascades, compare sampler bound by the shadow pass
layout(binding = 17) uniform sampler2DArrayShadow shadowCascades;

struct LightData {
    vec3  Position;  uint Type;
    vec3  Direction; float Intensity;
    vec3  Color;     float Range;
    float SpotInnerCos; float SpotOuterCos;
    float AreaWidth;    float AreaHeight;
};

// Lights[0] is the main directional light when one exists
layout(std140, binding = 2) uniform LightUBO {
    vec3  CameraPos;
    uint  NumActiveLights;
    vec3  AmbientColor;
    float AmbientIntensity;
    vec4  FogColor;            // horizon
    float FogStart;
    float FogEnd;
    float _lightPad0, _lightPad1;
    LightData Lights[16];
    vec4  FogColorZenith;      // zenith
    float HeightFogTop;
    float HeightFogDensity;
    float HeightFogFalloff;
    float HeightFogEnabled;
};

layout(std140, binding = 5) uniform ShadowUBO {
    mat4  ShadowLightVP;
    float ShadowBias;
    float ShadowStrength;
    float ShadowTexelSize;
    float ShadowLightSize;
    uint  PCSSEnabled;
    uint  ShadowMapEnabled;
    float _shadowPad0, _shadowPad1;
    mat4  NdcToShadow;
    mat4  CascadeVP[4];
    vec4  CascadeSplits;       // cascade radius around CameraPos
    uint  CascadeCount;
    float CascadeBlend;        // blend band, fraction of the radius
};

layout(std140, binding = 6) uniform DeferredCB {
    mat4  InvViewProj;
    float ScreenWidth;
    float ScreenHeight;
    float NearPlane;
    float FarPlane;
};

layout(std140, binding = 10) uniform IBLUBO {
    uint  IBLEnabled;
    float IBLIntensity;
    float MaxReflectionLOD;
    uint  _iblPad0;
};

// Clustered lights: lights[] holds ClusterGrid.w extra directional lights,
// then the local lights that cells[] (offset, count) index into.
layout(std140, binding = 7) uniform ClusterParams {
    vec4  ViewZ;               // view depth = dot(xyz, worldPos) + w
    uvec4 ClusterGrid;         // xyz = tiles x, tiles y, slices; w = globals
    float ClusterZScale;       // slice = log(depth) * scale + bias
    float ClusterZBias;
    uint  LocalLightCount;
};
layout(std430, binding = 0) readonly buffer ClusterLights {
    LightData lights[];
};
layout(std430, binding = 1) readonly buffer ClusterCells {
    uvec2 cells[];
};
layout(std430, binding = 2) readonly buffer ClusterLightIndices {
    uint lightIndices[];
};

const float PI = 3.14159265359;

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

vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0)
             * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ---- Shadow ----
float CascadeShadow(uint c, vec3 worldPos) {
    vec4 sc   = CascadeVP[c] * vec4(worldPos, 1.0);
    vec3 proj = sc.xyz / sc.w;
    proj.xy   = proj.xy * 0.5 + 0.5;
    proj.z    = proj.z  * 0.5 + 0.5;   // [-1,1] -> [0,1] for OpenGL

    if (proj.z > 1.0 || any(lessThan(proj.xy, vec2(0.0))) ||
        any(greaterThan(proj.xy, vec2(1.0))))
        return 1.0;

    float shadow = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec2 uv = proj.xy + poissonDisk[i] * ShadowTexelSize * 5.0;
        shadow += texture(shadowCascades,
                          vec4(uv, float(c), proj.z - ShadowBias));
    }
    return shadow / 16.0;
}

// Picks the cascade by distance from the camera and blends into the next one
// (or out to unshadowed after the last) across the band at its outer edge.
float CalcShadow(vec3 worldPos, float NdotL) {
    if (ShadowMapEnabled == 0u || CascadeCount == 0u) return 1.0;
    float dist = length(worldPos - CameraPos);
    uint  c    = 0u;
    while (c + 1u < CascadeCount && dist > CascadeSplits[c]) ++c;

    float outer = CascadeSplits[c];
    if (dist >= outer) return 1.0;
    float t      = smoothstep(outer * (1.0 - CascadeBlend), outer, dist);
    float shadow = CascadeShadow(c, worldPos);
    if (t > 0.0) {
        float next = (c + 1u < CascadeCount) ? CascadeShadow(c + 1u, worldPos)
                                             : 1.0;
        shadow = mix(shadow, next, t);
    }
    shadow *= smoothstep(0.0, 0.15, NdotL);
    return mix(1.0, shadow, ShadowStrength);
}

// ---- Clustered lights ----
uint ClusterIndex(vec2 ndcXY, vec3 worldPos) {
    float viewZ = dot(ViewZ.xyz, worldPos) + ViewZ.w;
    float slice = log(max(viewZ, 1e-3)) * ClusterZScale + ClusterZBias;
    uint  z     = uint(clamp(slice, 0.0, float(ClusterGrid.z - 1u)));
    vec2  tile  = clamp((ndcXY * 0.5 + 0.5) * vec2(ClusterGrid.xy),
                        vec2(0.0), vec2(ClusterGrid.xy) - 1.0);
    return (z * ClusterGrid.y + uint(tile.y)) * ClusterGrid.x + uint(tile.x);
}

// Windowed inverse-square falloff that reaches zero at the light's range,
// which is what lets the CPU cull it per cluster. Spots add the cone.
vec3 LocalLightRadiance(LightData light, vec3 worldPos, out vec3 L) {
    vec3  toLight = light.Position - worldPos;
    float dist    = length(toLight);
    L             = toLight / max(dist, 1e-4);
    float d       = dist / max(light.Range, 1e-4);
    float window  = clamp(1.0 - d * d * d * d, 0.0, 1.0);
    float atten   = window * window / (dist * dist + 1.0);
    if (light.Type == 2u)
        atten *= smoothstep(light.SpotOuterCos, light.SpotInnerCos,
                            dot(-L, normalize(light.Direction)));
    return light.Color * light.Intensity * atten;
}

// OpenGL: no vertex Y-flip and [-1,1] clip depth, so NDC.z = 2*depth-1.
vec3 ReconstructWorldPos(vec2 uv, float depth) {
    vec4 ndc   = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = InvViewProj * ndc;
    return world.xyz / world.w;
}

vec3 ACESFilm(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14),
                 0.0, 1.0);
}

vec3 ApplyFog(vec3 color, vec3 worldPos) {
    if (FogEnd <= 0.0) return color;
    vec3  toFrag  = worldPos - CameraPos;
    float dist    = length(toFrag.xz);
    float distFog = clamp((dist - FogStart) / max(FogEnd - FogStart, 1e-4),
                          0.0, 1.0);
    float heightFog = 0.0;
    if (HeightFogEnabled > 0.5) {
        float h = max(HeightFogTop - worldPos.y, 0.0);
        heightFog = clamp(HeightFogDensity * (1.0 - exp(-h * HeightFogFalloff)),
                          0.0, 1.0);
    }
    float fogAmount = 1.0 - (1.0 - distFog) * (1.0 - heightFog);
    vec3  viewDir   = normalize(toFrag);
    float t = smoothstep(0.0, 1.0, clamp(viewDir.y * 0.5 + 0.5, 0.0, 1.0));
    vec3  fogColor  = mix(FogColor.rgb, FogColorZenith.rgb, t);
    return mix(color, fogColor, fogAmount);
}

void main() {
    vec4  albedoAO    = texture(gbAlbedoAO,    fragUV);
    vec4  normalRough = texture(gbNormalRough, fragUV);
    vec4  metalEmit   = texture(gbMetalEmit,   fragUV);
    float depth       = texture(gbDepth,       fragUV).r;

    // Sky: leave depth at 1.0 for the skybox pass
    if (depth >= 1.0) discard;

    // The forward transparent pass depth-tests against this
    gl_FragDepth = depth;

    vec3  albedo    = albedoAO.rgb;
    float ao        = albedoAO.a;
    if (uSSAOEnabled != 0)
        ao *= 0.5 + 0.5 * texture(gbSSAO, fragUV).r;
    vec3  N         = normalize(normalRough.rgb * 2.0 - 1.0);
    float roughness = normalRough.a;
    float metallic  = metalEmit.r;
    float emitScale = metalEmit.g;

    vec3 worldPos = ReconstructWorldPos(fragUV, depth);

    // Wrap diffuse from the main directional light, plain Lambert elsewhere
    vec3 totalDiffuse = vec3(0.0);
    if (NumActiveLights > 0u && Lights[0].Type == 0u) {
        vec3  L        = normalize(-Lights[0].Direction);
        float rawNdotL = dot(N, L);
        float NdotL    = clamp(rawNdotL * 0.85 + 0.15, 0.0, 1.0);
        float shadow   = CalcShadow(worldPos, max(rawNdotL, 0.0));
        totalDiffuse  += Lights[0].Color * Lights[0].Intensity * NdotL * shadow;
    }
    for (uint i = 0u; i < ClusterGrid.w; ++i) {
        vec3 L = normalize(-lights[i].Direction);
        totalDiffuse += lights[i].Color * lights[i].Intensity *
                        max(dot(N, L), 0.0);
    }
    uvec2 cell = cells[ClusterIndex(fragUV * 2.0 - 1.0, worldPos)];
    for (uint i = 0u; i < cell.y; ++i) {
        vec3 L;
        vec3 radiance = LocalLightRadiance(lights[lightIndices[cell.x + i]],
                                           worldPos, L);
        totalDiffuse += radiance * max(dot(N, L), 0.0);
    }

    // Hemisphere ambient, with IBL added on top when it is enabled
    vec3  V             = normalize(CameraPos - worldPos);
    vec3  F0            = mix(vec3(0.04), albedo, metallic);
    vec3  skyAmbient    = AmbientColor * AmbientIntensity;
    vec3  groundAmbient = skyAmbient * vec3(0.55, 0.50, 0.45);
    vec3  ambient = albedo * mix(groundAmbient, skyAmbient, N.y * 0.5 + 0.5);

    if (IBLEnabled != 0u) {
        float NdotV       = max(dot(N, V), 0.0);
        vec3  kS          = FresnelSchlickRoughness(NdotV, F0, roughness);
        vec3  kD          = (1.0 - kS) * (1.0 - metallic);
        vec3  irradiance  = texture(irradianceMap, N).rgb;
        vec3  prefiltered = textureLod(prefilterMap, reflect(-V, N),
                                       roughness * MaxReflectionLOD).rgb;
        vec2  envBRDF     = texture(brdfLUT, vec2(NdotV, roughness)).rg;
        ambient += (kD * irradiance * albedo +
                    prefiltered * (kS * envBRDF.x + envBRDF.y)) * IBLIntensity;
    }
    ambient *= ao;

    vec3 color = ambient + albedo * totalDiffuse + albedo * emitScale;
    color = ACESFilm(color);
    color = ApplyFog(color, worldPos);
    outColor = vec4(color, 1.0);
}
