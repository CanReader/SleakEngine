#version 450

// ============================================================
// DXCraft Basic Material Shader - Vulkan Fragment Shader
// Hardcoded directional light (Vulkan renderer does not yet
// support Material/Light UBOs or multi-texture descriptor sets)
// ============================================================

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragWorldNorm;
layout(location = 2) in vec3 fragWorldTan;
layout(location = 3) in vec3 fragWorldBit;
layout(location = 4) in vec4 fragColor;
layout(location = 5) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

// Texture sampler (descriptor set 0, first texture only)
layout(set = 0, binding = 0) uniform sampler2D diffuseTexture;

void main() {
    // Sample the diffuse texture (first texture loaded by material)
    vec4 texColor = texture(diffuseTexture, fragUV);
    vec4 baseColor = texColor * fragColor;

    // Hardcoded directional light (sun-like)
    vec3 lightDir = normalize(vec3(-0.3, -1.0, -0.4));
    vec3 lightColor = vec3(1.0, 0.95, 0.9);
    float lightIntensity = 1.2;

    // Ambient
    vec3 ambient = vec3(0.15, 0.15, 0.18);

    // Diffuse (Lambert)
    vec3 N = normalize(fragWorldNorm);
    float NdotL = max(dot(N, -lightDir), 0.0);
    vec3 diffuse = lightColor * lightIntensity * NdotL;

    // Simple specular (Blinn-Phong)
    vec3 viewDir = normalize(-fragWorldPos);
    vec3 halfDir = normalize(-lightDir + viewDir);
    float spec = pow(max(dot(N, halfDir), 0.0), 32.0);
    vec3 specular = lightColor * spec * 0.3;

    vec3 finalColor = baseColor.rgb * (ambient + diffuse)
                    + specular;

    outColor = vec4(finalColor, baseColor.a);
}
