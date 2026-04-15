#version 450 core

// ============================================================
// DXCraft Basic Material Shader - OpenGL Fragment Shader
// Cook-Torrance PBR with multi-light support
// ============================================================

in vec3 fragWorldPos;
in vec3 fragWorldNorm;
in vec3 fragWorldTan;
in vec3 fragWorldBit;
in vec4 fragColor;
in vec2 fragUV;
in vec4 fragShadowCoord;

out vec4 outColor;

// Material UBO (binding 1) - matches MaterialGPUData layout
layout(std140, binding = 1) uniform MaterialUBO {
    // Row 0: texture presence flags
    uint HasDiffuseMap;
    uint HasNormalMap;
    uint HasSpecularMap;
    uint HasRoughnessMap;

    // Row 1: more flags
    uint HasMetallicMap;
    uint HasAOMap;
    uint HasEmissiveMap;
    uint _pad0;

    // Row 2: diffuse color RGBA
    vec4 matDiffuseColor;

    // Row 3: specular RGB + shininess
    vec3 matSpecularColor;
    float matShininess;

    // Row 4: emissive RGB + intensity
    vec3 matEmissiveColor;
    float matEmissiveIntensity;

    // Row 5: PBR factors
    float matMetallic;
    float matRoughness;
    float matAO;
    float matNormalIntensity;

    // Row 6: UV transform
    vec2 matTiling;
    vec2 matOffset;

    // Row 7: alpha
    float matOpacity;
    float matAlphaCutoff;
    vec2 _pad1;
};

struct LightData {
    vec3 Position;  uint Type;
    vec3 Direction; float Intensity;
    vec3 Color;     float Range;
    float SpotInnerCos; float SpotOuterCos;
    float AreaWidth; float AreaHeight;
};

layout(std140, binding = 2) uniform LightUBO {
    vec3 CameraPos;
    uint NumActiveLights;
    vec3 AmbientColor;
    float AmbientIntensity;
    vec4 FogColor;             // horizon
    float FogStart;
    float FogEnd;
    float _lightPad0, _lightPad1;
    LightData Lights[16];
    // Trailing block — must match LightCBData order in ConstantBuffer.hpp.
    vec4 FogColorZenith;       // zenith
    float HeightFogTop;
    float HeightFogDensity;
    float HeightFogFalloff;
    float HeightFogEnabled;
};

// Texture samplers — binding must match TEXTURE_SLOT defines
layout(binding = 0) uniform sampler2D diffuseTexture;
layout(binding = 1) uniform sampler2D normalTexture;
layout(binding = 2) uniform sampler2D specularTexture;
layout(binding = 3) uniform sampler2D roughnessTexture;
layout(binding = 4) uniform sampler2D metallicTexture;
layout(binding = 5) uniform sampler2D aoTexture;
layout(binding = 6) uniform sampler2D emissiveTexture;

const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = PI * denom * denom;

    return a2 / max(denom, 0.0000001);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;

    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(
        clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Fresnel-Schlick with roughness (for IBL ambient specular)
vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    float oneMinusRoughness = 1.0 - roughness;
    return F0 + (max(vec3(oneMinusRoughness), F0) - F0)
        * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// IBL textures
layout(binding = 7) uniform samplerCube irradianceMap;
layout(binding = 8) uniform samplerCube prefilterMap;
layout(binding = 9) uniform sampler2D   brdfLUT;

layout(std140, binding = 3) uniform IBLUBO {
    uint  IBLEnabled;
    float IBLIntensity;
    float MaxReflectionLOD;
    uint  _iblPad0;
};

// SSAO composite texture (screen-space, blurred AO)
layout(binding = 10) uniform sampler2D ssaoTexture;

layout(std140, binding = 4) uniform SSAOUBO_Composite {
    uint  SSAOCompositeEnabled;
    float ssaoScreenWidth;
    float ssaoScreenHeight;
    float _ssaoCompPad0;
};

// Shadow mapping with PCSS
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

layout(binding = 11) uniform sampler2DShadow shadowMapSampler;
layout(binding = 12) uniform sampler2D       shadowMapDepth;

// ---- PCSS Shadow Functions ----

float InterleavedGradientNoiseGL(vec2 screenPos) {
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(screenPos, magic.xy)));
}

const vec2 poissonDisk[32] = vec2[](
    vec2(-0.9465, -0.1484), vec2(-0.7431,  0.5353),
    vec2(-0.5863, -0.5879), vec2(-0.3935,  0.1025),
    vec2(-0.2428,  0.7722), vec2(-0.1074, -0.3075),
    vec2( 0.0542, -0.8645), vec2( 0.1267,  0.4300),
    vec2( 0.2787, -0.1353), vec2( 0.3842,  0.6501),
    vec2( 0.4714, -0.5537), vec2( 0.5765,  0.1675),
    vec2( 0.6712, -0.3340), vec2( 0.7527,  0.4813),
    vec2( 0.8745, -0.0910), vec2( 0.9601,  0.2637),
    vec2(-0.8312,  0.3150), vec2(-0.6142, -0.2890),
    vec2(-0.4581,  0.6310), vec2(-0.2134, -0.7520),
    vec2(-0.0678,  0.4210), vec2( 0.1893, -0.5430),
    vec2( 0.3215,  0.8140), vec2( 0.4890, -0.0230),
    vec2( 0.6340,  0.3470), vec2( 0.7810, -0.5690),
    vec2(-0.3467,  0.2560), vec2(-0.1290, -0.1120),
    vec2( 0.0910,  0.1560), vec2( 0.2340, -0.3410),
    vec2(-0.5120,  0.0420), vec2( 0.4210,  0.5120)
);

float FindBlockerDepthGL(vec2 uv, float receiverDepth, float searchRadius) {
    float blockerSum = 0.0;
    int blockerCount = 0;

    float angle = InterleavedGradientNoiseGL(gl_FragCoord.xy) * 6.283185;
    float sa = sin(angle);
    float ca = cos(angle);
    mat2 rotation = mat2(ca, sa, -sa, ca);

    for (int i = 0; i < 16; i++) {
        vec2 offset = rotation * poissonDisk[i] * searchRadius;
        float sampleDepth = texture(shadowMapDepth, uv + offset).r;
        if (sampleDepth < receiverDepth) {
            blockerSum += sampleDepth;
            blockerCount++;
        }
    }

    if (blockerCount == 0) return -1.0;
    return blockerSum / float(blockerCount);
}

float PCSSFilterGL(vec2 uv, float receiverDepth, float filterRadius) {
    float angle = InterleavedGradientNoiseGL(gl_FragCoord.xy) * 6.283185;
    float sa = sin(angle);
    float ca = cos(angle);
    mat2 rotation = mat2(ca, sa, -sa, ca);

    float shadow = 0.0;
    for (int i = 0; i < 32; i++) {
        vec2 offset = rotation * poissonDisk[i] * filterRadius;
        shadow += texture(shadowMapSampler, vec3(uv + offset, receiverDepth));
    }
    return shadow / 32.0;
}

float CalcShadowPCSSGL(vec4 sc) {
    if (ShadowMapEnabled == 0u) return 1.0;

    vec3 proj = sc.xyz / sc.w;
    proj.xy = proj.xy * 0.5 + 0.5;

    if (proj.x < 0.0 || proj.x > 1.0 ||
        proj.y < 0.0 || proj.y > 1.0 ||
        proj.z < 0.0 || proj.z > 1.0)
        return 1.0;

    vec2 fadeCoord = smoothstep(vec2(0.0), vec2(0.05), proj.xy)
                   * smoothstep(vec2(0.0), vec2(0.05), vec2(1.0) - proj.xy);
    float edgeFade = fadeCoord.x * fadeCoord.y;

    float biasedDepth = proj.z - ShadowBias;
    float shadow;

    if (PCSSEnabled != 0u) {
        float searchRadius = ShadowTexelSize * ShadowLightSize * 40.0;
        float blockerDepth = FindBlockerDepthGL(proj.xy, biasedDepth, searchRadius);

        if (blockerDepth < 0.0) return 1.0;

        float penumbra = ((biasedDepth - blockerDepth) / blockerDepth) * ShadowLightSize;
        float filterRadius = max(penumbra * ShadowTexelSize * 80.0, ShadowTexelSize * 8.0);
        filterRadius = min(filterRadius, ShadowTexelSize * ShadowLightSize * 30.0);

        shadow = PCSSFilterGL(proj.xy, biasedDepth, filterRadius);
    } else {
        // Standard 3x3 PCF
        shadow = 0.0;
        float radius = ShadowTexelSize * 2.0;
        for (int x = -1; x <= 1; x++) {
            for (int y = -1; y <= 1; y++) {
                vec2 offset = vec2(float(x), float(y)) * radius;
                shadow += texture(shadowMapSampler, vec3(proj.xy + offset, biasedDepth));
            }
        }
        shadow /= 9.0;
    }

    return mix(1.0, shadow, ShadowStrength * edgeFade);
}

// ---- End PCSS Shadow Functions ----

float AttenuateUE4(float distance, float range) {
    if (range <= 0.0) return 1.0;
    float d = distance / range;
    float d2 = d * d;
    float d4 = d2 * d2;
    float falloff = clamp(1.0 - d4, 0.0, 1.0);
    return (falloff * falloff) / (distance * distance + 1.0);
}

// Sky-matched gradient fog + exponential height fog. View direction picks a
// blend between horizon and zenith colors so distant terrain dissolves into
// the same color the sky renders behind it.
vec3 ApplyFog(vec3 color, vec3 worldPos) {
    if (FogEnd <= 0.0) return color;
    vec3 toFrag = worldPos - CameraPos;
    float dist = length(toFrag.xz);
    float distFog = clamp((dist - FogStart) / max(FogEnd - FogStart, 1e-4), 0.0, 1.0);
    float heightFog = 0.0;
    if (HeightFogEnabled > 0.5) {
        float h = max(HeightFogTop - worldPos.y, 0.0);
        heightFog = clamp(HeightFogDensity * (1.0 - exp(-h * HeightFogFalloff)),
                          0.0, 1.0);
    }
    float fogAmount = 1.0 - (1.0 - distFog) * (1.0 - heightFog);
    vec3 viewDir = normalize(toFrag);
    float t = smoothstep(0.0, 1.0, clamp(viewDir.y * 0.5 + 0.5, 0.0, 1.0));
    vec3 fogColor = mix(FogColor.rgb, FogColorZenith.rgb, t);
    return mix(color, fogColor, fogAmount);
}

void main() {
    // Apply UV tiling and offset
    vec2 uv = fragUV * matTiling + matOffset;

    vec4 baseColor = matDiffuseColor * fragColor;
    if (HasDiffuseMap != 0u)
        baseColor *= texture(diffuseTexture, uv);

    float alpha = baseColor.a * matOpacity;
    if (matAlphaCutoff > 0.0 && alpha < matAlphaCutoff)
        discard;

    vec3 N = normalize(fragWorldNorm);
    if (HasNormalMap != 0u) {
        vec3 tangentNormal =
            texture(normalTexture, uv).xyz * 2.0 - 1.0;
        tangentNormal.xy *= matNormalIntensity;
        tangentNormal = normalize(tangentNormal);

        vec3 T = normalize(fragWorldTan);
        vec3 B = normalize(fragWorldBit);
        mat3 TBN = mat3(T, B, N);
        N = normalize(TBN * tangentNormal);
    }

    float roughness = matRoughness;
    if (HasRoughnessMap != 0u)
        roughness *= texture(roughnessTexture, uv).r;
    roughness = clamp(roughness, 0.04, 1.0);

    float metallic = matMetallic;
    if (HasMetallicMap != 0u)
        metallic *= texture(metallicTexture, uv).r;

    float ao = matAO;
    if (HasAOMap != 0u)
        ao *= texture(aoTexture, uv).r;

    // Composite screen-space AO
    if (SSAOCompositeEnabled != 0u) {
        vec2 screenUV = gl_FragCoord.xy / vec2(ssaoScreenWidth, ssaoScreenHeight);
        float ssao = texture(ssaoTexture, screenUV).r;
        ao *= ssao;
    }

    vec3 albedo = baseColor.rgb;
    vec3 V = normalize(CameraPos - fragWorldPos);

    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);

    vec3 Lo = vec3(0.0);

    for (uint i = 0u; i < NumActiveLights; i++) {
        LightData light = Lights[i];

        vec3 L;
        float attenuation = 1.0;

        if (light.Type == 0u) {
            L = normalize(-light.Direction);
            // Apply PCSS shadow to first directional light
            if (i == 0u) {
                float shadow = CalcShadowPCSSGL(fragShadowCoord);
                attenuation *= shadow;
            }
        }
        else if (light.Type == 1u) {
            vec3 toLight = light.Position - fragWorldPos;
            float dist = length(toLight);
            L = toLight / max(dist, 0.0001);
            attenuation = AttenuateUE4(dist, light.Range);
        }
        else if (light.Type == 2u) {
            vec3 toLight = light.Position - fragWorldPos;
            float dist = length(toLight);
            L = toLight / max(dist, 0.0001);
            attenuation = AttenuateUE4(dist, light.Range);

            float theta = dot(L, normalize(-light.Direction));
            float epsilon = light.SpotInnerCos
                          - light.SpotOuterCos;
            float spotFactor = clamp(
                (theta - light.SpotOuterCos)
                / max(epsilon, 0.0001), 0.0, 1.0);
            attenuation *= spotFactor;
        }
        else {
            vec3 toLight = light.Position - fragWorldPos;
            float dist = length(toLight);
            L = toLight / max(dist, 0.0001);
            attenuation = AttenuateUE4(dist, light.Range);
        }

        vec3 H = normalize(V + L);
        float NdotL = max(dot(N, L), 0.0);

        if (NdotL > 0.0) {
            float  D = DistributionGGX(N, H, roughness);
            float  G = GeometrySmith(N, V, L, roughness);
            vec3   F = FresnelSchlick(
                max(dot(H, V), 0.0), F0);

            vec3 numerator = D * G * F;
            float denominator = 4.0 * max(dot(N, V), 0.0)
                              * NdotL + 0.0001;
            vec3 specular = numerator / denominator;

            vec3 kS = F;
            vec3 kD = (1.0 - kS) * (1.0 - metallic);

            vec3 radiance = light.Color * light.Intensity
                          * attenuation;

            Lo += (kD * albedo / PI + specular)
                * radiance * NdotL;
        }
    }

    vec3 ambient;
    if (IBLEnabled != 0u) {
        // IBL diffuse: sample irradiance map
        vec3 kS_ibl = FresnelSchlickRoughness(
            max(dot(N, V), 0.0), F0, roughness);
        vec3 kD_ibl = (1.0 - kS_ibl) * (1.0 - metallic);

        vec3 irradiance = texture(irradianceMap, N).rgb;
        vec3 diffuseIBL = irradiance * albedo;

        // IBL specular: sample prefiltered env map + BRDF LUT
        vec3 R = reflect(-V, N);
        vec3 prefilteredColor = textureLod(
            prefilterMap, R, roughness * MaxReflectionLOD).rgb;
        float NdotV_ibl = max(dot(N, V), 0.0);
        vec2 envBRDF = texture(brdfLUT,
            vec2(NdotV_ibl, roughness)).rg;
        vec3 specularIBL = prefilteredColor
            * (kS_ibl * envBRDF.x + envBRDF.y);

        ambient = (kD_ibl * diffuseIBL + specularIBL)
                * ao * IBLIntensity;
    } else {
        ambient = AmbientColor * AmbientIntensity
                * albedo * ao;
    }

    vec3 emissive = matEmissiveColor * matEmissiveIntensity;
    if (HasEmissiveMap != 0u)
        emissive *= texture(emissiveTexture, uv).rgb;

    vec3 finalColor = ambient + Lo + emissive;

    finalColor = ApplyFog(finalColor, fragWorldPos);

    outColor = vec4(finalColor, alpha);
}
