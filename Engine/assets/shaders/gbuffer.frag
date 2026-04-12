#version 450

// ============================================================
// GBuffer Geometry Pass - Vulkan Fragment Shader
// Writes material data into 3 MRT attachments:
//   RT0 (location=0): AlbedoAO   — albedo.rgb + AO in alpha
//   RT1 (location=1): NormalRough — world normal.xyz + roughness in alpha
//   RT2 (location=2): MetalEmit   — metallic in R, emissive scale in G
// ============================================================

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragWorldNorm;
layout(location = 2) in vec3 fragWorldTan;
layout(location = 3) in vec3 fragWorldBit;
layout(location = 4) in vec4 fragColor;
layout(location = 5) in vec2 fragUV;
layout(location = 6) in vec4 fragShadowCoord;

// Set 0: diffuse texture (same binding as forward pipeline)
layout(set = 0, binding = 0) uniform sampler2D diffuseTexture;

// Output attachments
layout(location = 0) out vec4 outAlbedoAO;     // RT0
layout(location = 1) out vec4 outNormalRough;  // RT1
layout(location = 2) out vec4 outMetalEmit;    // RT2

void main() {
    vec4 texColor = texture(diffuseTexture, fragUV);

    // Alpha discard (same threshold as forward pass)
    if (texColor.a < 0.5)
        discard;

    // AO is stored in vertex color red channel (same convention as forward shader)
    float ao = fragColor.r;

    // Albedo + AO
    outAlbedoAO = vec4(texColor.rgb, ao);

    // Encode world-space normal (remapped to [0,1]) + default roughness 0.8
    vec3 N = normalize(fragWorldNorm);
    outNormalRough = vec4(N * 0.5 + 0.5, 0.8);

    // Metallic = 0 (non-metallic geometry), emissive scale = 0
    outMetalEmit = vec4(0.0, 0.0, 0.0, 1.0);
}
