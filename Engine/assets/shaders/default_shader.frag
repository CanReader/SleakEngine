#version 450

// ============================================================
// Default Material Shader - Vulkan Fragment Shader
// Hemisphere ambient + Fresnel + smooth PCF shadows
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
};

layout(set = 3, binding = 0) uniform sampler2DShadow shadowMap;

// Interleaved gradient noise for per-pixel disk rotation
float InterleavedGradientNoise(vec2 screenPos) {
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(screenPos, magic.xy)));
}

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

    vec3 N = normalize(fragWorldNorm);
    vec3 lightDir = normalize(uLightDir.xyz);
    vec3 viewDir = normalize(uCameraPos.xyz - fragWorldPos);
    vec3 lightColor = uLightColor.rgb;
    float lightIntensity = uLightColor.a;

    // ---- Ambient ----
    vec3 ambientColor = uAmbient.rgb * uAmbient.a;
    // Subtle hemisphere: ground bounce slightly warmer/darker than sky
    vec3 groundColor = ambientColor * vec3(0.7, 0.65, 0.6);
    float hemisphere = N.y * 0.5 + 0.5;
    vec3 ambient = mix(groundColor, ambientColor, hemisphere);

    // ---- Diffuse (Lambert) ----
    float NdotL = dot(N, -lightDir);
    float diffuseTerm = max(NdotL, 0.0);
    vec3 diffuse = lightColor * lightIntensity * diffuseTerm;

    // ---- Specular (Blinn-Phong) ----
    vec3 halfDir = normalize(-lightDir + viewDir);
    float shininess = 32.0;
    float normFactor = (shininess + 8.0) / 25.1327;
    float spec = normFactor * pow(max(dot(N, halfDir), 0.0), shininess);
    vec3 specular = lightColor * spec * 0.5 * max(NdotL, 0.0);

    // ---- Shadow ----
    float shadow = CalcShadow(fragShadowCoord);

    // ---- Compose ----
    vec3 lit = baseColor.rgb * (ambient + shadow * diffuse)
             + shadow * specular;

    // ---- Tone mapping (Reinhard) ----
    lit = lit / (lit + vec3(1.0));

    outColor = vec4(lit, baseColor.a);
}
