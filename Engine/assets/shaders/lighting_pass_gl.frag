#version 450 core

// Deferred lighting pass.
// Reads the 4 GBuffer textures (bound at units 8-11),
// reconstructs world position from depth via InvViewProj,
// and computes Cook-Torrance PBR lighting + PCF shadows.

in  vec2 fragUV;
out vec4 outColor;

// GBuffer samplers (bound at texture units 8-11)
layout(binding = 8)  uniform sampler2D gbAlbedoAO;
layout(binding = 9)  uniform sampler2D gbNormalRough;
layout(binding = 10) uniform sampler2D gbMetalEmit;
layout(binding = 11) uniform sampler2D gbDepth;

// Shadow map (already bound at unit 3 by the shadow pass)
layout(binding = 3) uniform sampler2DShadow shadowMap;

// ---- UBOs ----
struct LightData {
    vec3  Position;  uint Type;
    vec3  Direction; float Intensity;
    vec3  Color;     float Range;
    float SpotInnerCos; float SpotOuterCos;
    float AreaWidth;    float AreaHeight;
};

layout(std140, binding = 2) uniform LightUBO {
    vec3  CameraPos;
    uint  NumActiveLights;
    vec3  AmbientColor;
    float AmbientIntensity;
    vec4  FogColor;
    float FogStart;
    float FogEnd;
    float _lightPad0, _lightPad1;
    LightData Lights[16];
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
};

layout(std140, binding = 6) uniform DeferredCB {
    mat4  InvViewProj;
    float ScreenWidth;
    float ScreenHeight;
    float NearPlane;
    float FarPlane;
};

// ---- Constants ----
const float PI = 3.14159265359;

// Poisson disk for PCF
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

// ---- PBR helper functions ----
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom  = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1e-7);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-7);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

// ---- Shadow ----
float CalcShadowPCF(vec3 worldPos, float NdotL) {
    if (ShadowMapEnabled == 0u) return 1.0;

    vec4 sc = ShadowLightVP * vec4(worldPos, 1.0);
    vec3 proj = sc.xyz / sc.w;
    proj.xy   = proj.xy * 0.5 + 0.5;
    proj.z    = proj.z  * 0.5 + 0.5;   // remap [-1,1]->  [0,1] for OpenGL

    if (proj.z > 1.0 || any(lessThan(proj.xy, vec2(0.0))) || any(greaterThan(proj.xy, vec2(1.0))))
        return 1.0;

    vec2 fade = smoothstep(vec2(0.0), vec2(0.05), proj.xy)
              * smoothstep(vec2(0.0), vec2(0.05), 1.0 - proj.xy);
    float edgeFade = fade.x * fade.y;

    float shadow = 0.0;
    for (int i = 0; i < 16; ++i) {
        shadow += texture(shadowMap,
            vec3(proj.xy + poissonDisk[i] * ShadowTexelSize * 1.5,
                 proj.z - ShadowBias));
    }
    shadow /= 16.0;

    // Fade shadow on surfaces facing away from light
    float fadedShadow = shadow * smoothstep(0.0, 0.15, NdotL);
    return mix(1.0, fadedShadow, ShadowStrength * edgeFade);
}

// ---- ACES tone mapping ----
vec3 ACESFilm(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    // ---- Sample GBuffer ----
    vec4  albedoAO     = texture(gbAlbedoAO,    fragUV);
    vec4  normalRough  = texture(gbNormalRough,  fragUV);
    vec4  metalEmit    = texture(gbMetalEmit,    fragUV);
    float depth        = texture(gbDepth,         fragUV).r;

    // Sky pixel — skip lighting
    if (depth >= 1.0) discard;

    vec3  albedo    = albedoAO.rgb;
    float ao        = albedoAO.a;
    vec3  N         = normalize(normalRough.rgb * 2.0 - 1.0);  // unpack from [0,1]
    float roughness = normalRough.a;
    float metallic  = metalEmit.r;
    float emitScale = metalEmit.g;

    // ---- Reconstruct world position ----
    vec4 ndcPos  = vec4(fragUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 worldH  = InvViewProj * ndcPos;
    vec3 worldPos = worldH.xyz / worldH.w;

    vec3 V  = normalize(CameraPos - worldPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // ---- PBR lighting accumulation ----
    vec3 Lo = vec3(0.0);

    for (uint i = 0u; i < NumActiveLights; ++i) {
        LightData light = Lights[i];

        vec3 L;
        float attenuation = 1.0;

        if (light.Type == 0u) {
            // Directional
            L = normalize(-light.Direction);
        } else if (light.Type == 1u) {
            // Point
            vec3 toLight = light.Position - worldPos;
            float dist   = length(toLight);
            L            = toLight / dist;
            attenuation  = 1.0 / (1.0 + dist * dist / max(light.Range * light.Range, 1e-4));
        } else {
            continue;  // spot/area: skip for now
        }

        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0) continue;

        float shadow = (light.Type == 0u) ? CalcShadowPCF(worldPos, NdotL) : 1.0;

        vec3 H       = normalize(V + L);
        vec3 radiance = light.Color * light.Intensity * attenuation;

        // Cook-Torrance specular
        float NDF = DistributionGGX(N, H, roughness);
        float G   = GeometrySmith(N, V, L, roughness);
        vec3  F   = FresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3  numerator   = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL;
        vec3  specular    = numerator / max(denominator, 1e-7);

        // Diffuse
        vec3 kD = (1.0 - F) * (1.0 - metallic);
        vec3 diffuse = kD * albedo / PI;

        Lo += (diffuse + specular) * radiance * NdotL * shadow;
    }

    // ---- Ambient + AO ----
    vec3 skyAmbient    = AmbientColor * AmbientIntensity;
    vec3 groundAmbient = skyAmbient * vec3(0.55, 0.50, 0.45);
    float hemisphere   = N.y * 0.5 + 0.5;
    vec3  ambient      = mix(groundAmbient, skyAmbient, hemisphere) * albedo * ao;

    vec3 color = ambient + Lo;

    // ---- Emissive ----
    color += albedo * emitScale;

    // ---- Tone mapping ----
    color = ACESFilm(color);

    // ---- Fog ----
    if (FogEnd > 0.0) {
        float dist      = length(worldPos - CameraPos);
        float fogFactor = clamp((FogEnd - dist) / max(FogEnd - FogStart, 1e-4), 0.0, 1.0);
        color           = mix(FogColor.rgb, color, fogFactor);
    }

    outColor = vec4(color, 1.0);
}
