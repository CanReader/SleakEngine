#version 450

// ============================================================
// Default Material Shader - Vulkan Fragment Shader
// Smooth PCF shadows with per-pixel rotated Poisson disk
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
};

layout(set = 3, binding = 0) uniform sampler2DShadow shadowMap;

// Interleaved gradient noise — gives a unique angle per screen pixel,
// breaks up PCF banding into imperceptible high-frequency grain.
float InterleavedGradientNoise(vec2 screenPos) {
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(screenPos, magic.xy)));
}

// 16-tap Poisson disk — rotated per pixel, with hardware bilinear PCF
// that gives 4 sub-samples each = 64 effective filtered comparisons
const vec2 disk[16] = vec2[](
    vec2(-0.9465, -0.1484), vec2(-0.7431,  0.5353),
    vec2(-0.5863, -0.5879), vec2(-0.3935,  0.1025),
    vec2(-0.2428,  0.7722), vec2(-0.1074, -0.3075),
    vec2( 0.0542, -0.8645), vec2( 0.1267,  0.4300),
    vec2( 0.2787, -0.1353), vec2( 0.3842,  0.6501),
    vec2( 0.4714, -0.5537), vec2( 0.5765,  0.1675),
    vec2( 0.6712, -0.3340), vec2( 0.7527,  0.4813),
    vec2( 0.8745, -0.0910), vec2( 0.9601,  0.2637)
);

float CalcShadow(vec4 sc) {
    vec3 projCoords = sc.xyz / sc.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z < 0.0 || projCoords.z > 1.0)
        return 1.0;

    float biasedDepth = projCoords.z - uShadowBias;

    // Per-pixel random rotation angle — eliminates banding
    float angle = InterleavedGradientNoise(gl_FragCoord.xy) * 6.283185;
    float sa = sin(angle);
    float ca = cos(angle);
    mat2 rotation = mat2(ca, sa, -sa, ca);

    float radius = uShadowTexelSize * uLightSize * 6.0;

    float shadow = 0.0;
    for (int i = 0; i < 16; i++) {
        vec2 offset = rotation * disk[i] * radius;
        shadow += texture(shadowMap, vec3(projCoords.xy + offset, biasedDepth));
    }
    shadow /= 16.0;

    return mix(1.0, shadow, uShadowStrength);
}

void main() {
    vec4 texColor = texture(diffuseTexture, fragUV);
    vec4 baseColor = texColor * fragColor;

    vec3 lightDir = normalize(uLightDir.xyz);
    vec3 lightColor = uLightColor.rgb;
    float lightIntensity = uLightColor.a;
    vec3 ambient = uAmbient.rgb * uAmbient.a;

    vec3 N = normalize(fragWorldNorm);
    float NdotL = max(dot(N, -lightDir), 0.0);
    vec3 diffuse = lightColor * lightIntensity * NdotL;

    vec3 viewDir = normalize(uCameraPos.xyz - fragWorldPos);
    vec3 halfDir = normalize(-lightDir + viewDir);
    float spec = pow(max(dot(N, halfDir), 0.0), 32.0);
    vec3 specular = lightColor * spec * 0.3;

    float shadow = CalcShadow(fragShadowCoord);

    vec3 finalColor = baseColor.rgb * (ambient + shadow * diffuse)
                    + shadow * specular;

    outColor = vec4(finalColor, baseColor.a);
}
