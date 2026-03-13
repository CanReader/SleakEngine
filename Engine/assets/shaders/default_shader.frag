#version 450

// ============================================================
// Default Material Shader - Vulkan Fragment Shader
// PCSS shadows + hemisphere ambient + per-vertex AO
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

// Interleaved gradient noise for per-pixel disk rotation
float InterleavedGradientNoise(vec2 screenPos) {
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(screenPos, magic.xy)));
}

// 32-sample Poisson disk for high quality PCF
const vec2 disk[32] = vec2[](
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

// PCSS Step 1: Find average blocker depth using first 16 samples
float FindBlockerDepth(vec2 uv, float receiverDepth, float searchRadius) {
    float blockerSum = 0.0;
    int blockerCount = 0;

    float angle = InterleavedGradientNoise(gl_FragCoord.xy) * 6.283185;
    float sa = sin(angle);
    float ca = cos(angle);
    mat2 rotation = mat2(ca, sa, -sa, ca);

    for (int i = 0; i < 16; i++) {
        vec2 offset = rotation * disk[i] * searchRadius;
        vec2 sampleUV = uv + offset;

        // Sample raw depth from shadow map (bypass comparison sampler)
        // Use a small bias for blocker search
        float sampleResult = texture(shadowMap, vec3(sampleUV, receiverDepth));
        if (sampleResult < 0.5) {
            // This sample is in shadow — it's a blocker
            // Estimate blocker depth as receiver depth (we can't read raw depth
            // with a comparison sampler, so we approximate)
            blockerSum += receiverDepth + uShadowBias * 2.0;
            blockerCount++;
        }
    }

    if (blockerCount == 0)
        return -1.0; // No blockers found

    return blockerSum / float(blockerCount);
}

// PCSS Step 2: Estimate penumbra width from blocker distance
float EstimatePenumbraWidth(float receiverDepth, float blockerDepth) {
    float penumbra = (receiverDepth - blockerDepth) / blockerDepth;
    return penumbra * uLightSize;
}

float CalcShadow(vec4 sc) {
    vec3 projCoords = sc.xyz / sc.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;

    // Out-of-bounds check
    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z < 0.0 || projCoords.z > 1.0)
        return 1.0;

    // Smooth fade at shadow map edges to avoid hard cutoffs
    vec2 fadeCoord = smoothstep(vec2(0.0), vec2(0.05), projCoords.xy)
                   * smoothstep(vec2(0.0), vec2(0.05), vec2(1.0) - projCoords.xy);
    float edgeFade = fadeCoord.x * fadeCoord.y;

    float biasedDepth = projCoords.z - uShadowBias;

    float angle = InterleavedGradientNoise(gl_FragCoord.xy) * 6.283185;
    float sa = sin(angle);
    float ca = cos(angle);
    mat2 rotation = mat2(ca, sa, -sa, ca);

    // PCSS: Blocker search with wide radius
    float searchRadius = uShadowTexelSize * uLightSize * 40.0;
    float blockerDepth = FindBlockerDepth(projCoords.xy, biasedDepth, searchRadius);

    // Determine filter radius based on blocker distance
    float filterRadius;
    if (blockerDepth < 0.0) {
        // No blockers — fully lit, skip expensive filtering
        return 1.0;
    } else {
        // PCSS penumbra estimation
        float penumbra = EstimatePenumbraWidth(biasedDepth, blockerDepth);
        filterRadius = max(penumbra * uShadowTexelSize * 80.0, uShadowTexelSize * 8.0);
        // Clamp to reasonable max to prevent artifacts
        filterRadius = min(filterRadius, uShadowTexelSize * uLightSize * 30.0);
    }

    // PCF filtering with 32 samples at computed radius
    float shadow = 0.0;
    for (int i = 0; i < 32; i++) {
        vec2 offset = rotation * disk[i] * filterRadius;
        shadow += texture(shadowMap, vec3(projCoords.xy + offset, biasedDepth));
    }
    shadow /= 32.0;

    // Apply edge fade and shadow strength
    shadow = mix(1.0, shadow, uShadowStrength * edgeFade);
    return shadow;
}

void main() {
    vec4 texColor = texture(diffuseTexture, fragUV);
    if (texColor.a < 0.5)
        discard;
    vec4 baseColor = texColor * fragColor;

    vec3 N = normalize(fragWorldNorm);
    vec3 lightDir = normalize(uLightDir.xyz);
    vec3 lightColor = uLightColor.rgb;
    float lightIntensity = uLightColor.a;

    // Hemisphere ambient: sky color above, ground-bounce below
    vec3 ambientColor = uAmbient.rgb * uAmbient.a;
    vec3 groundColor = ambientColor * vec3(0.65, 0.6, 0.55);
    float hemisphere = N.y * 0.5 + 0.5;
    vec3 ambient = mix(groundColor, ambientColor, hemisphere);

    // Diffuse lighting
    float NdotL = dot(N, -lightDir);
    float diffuseTerm = max(NdotL, 0.0);

    // Wrap lighting for softer light falloff on side faces
    float wrapTerm = max((NdotL + 0.15) / 1.15, 0.0);
    vec3 diffuse = lightColor * lightIntensity * wrapTerm;

    float shadow = CalcShadow(fragShadowCoord);

    vec3 lit = baseColor.rgb * (ambient + shadow * diffuse);

    // Reinhard tone mapping
    lit = lit / (lit + vec3(1.0));

    outColor = vec4(lit, baseColor.a);
}
