#version 450

// ============================================================
// Bloom Upsample — 9-tap tent filter (COD-style pyramid upsample).
// The pipeline is configured to additively blend onto the
// destination, so this shader outputs just the filtered source.
// ============================================================

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTex;

layout(push_constant) uniform BloomUpPC {
    vec2  srcTexelSize;  // 1.0 / sourceSize
    float filterRadius;  // scales the tent; typical 1.0
    float intensity;     // output multiplier for this level
};

void main() {
    vec2 ts = srcTexelSize * filterRadius;

    // 3x3 tent kernel (sum = 16; weights below divided by 16)
    //  1 2 1
    //  2 4 2
    //  1 2 1
    vec3 a = texture(srcTex, fragUV + vec2(-ts.x, -ts.y)).rgb;
    vec3 b = texture(srcTex, fragUV + vec2( 0.0,  -ts.y)).rgb;
    vec3 c = texture(srcTex, fragUV + vec2( ts.x, -ts.y)).rgb;

    vec3 d = texture(srcTex, fragUV + vec2(-ts.x,  0.0)).rgb;
    vec3 e = texture(srcTex, fragUV + vec2( 0.0,   0.0)).rgb;
    vec3 f = texture(srcTex, fragUV + vec2( ts.x,  0.0)).rgb;

    vec3 g = texture(srcTex, fragUV + vec2(-ts.x,  ts.y)).rgb;
    vec3 h = texture(srcTex, fragUV + vec2( 0.0,   ts.y)).rgb;
    vec3 i = texture(srcTex, fragUV + vec2( ts.x,  ts.y)).rgb;

    vec3 upsample = (e * 4.0 + (b + d + f + h) * 2.0 + (a + c + g + i)) / 16.0;
    outColor = vec4(upsample * intensity, 1.0);
}
