// ============================================================
// PCSS Soft Shadows (OpenGL GLSL)
// Include this in your fragment shader for PCSS support
// Requires: shadow map sampler, shadow comparison sampler
// ============================================================
//
// This is a standalone reference shader for the PCSS algorithm.
// To integrate, add these uniforms and functions to your
// main fragment shader.

// Shadow UBO layout (binding 5)
// layout(std140, binding = 5) uniform ShadowUBO {
//     mat4  ShadowLightVP;
//     vec4  ShadowLightDir;
//     vec4  ShadowLightColor;
//     float ShadowBias;
//     float ShadowStrength;
//     float ShadowTexelSize;
//     float ShadowLightSize;
//     uint  PCSSEnabled;
//     float _shadowPad0, _shadowPad1, _shadowPad2;
// };
//
// layout(binding = 11) uniform sampler2DShadow shadowMapSampler;
// layout(binding = 12) uniform sampler2D       shadowMapDepth;

// Interleaved gradient noise for per-pixel disk rotation
// float InterleavedGradientNoise(vec2 screenPos) {
//     vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
//     return fract(magic.z * fract(dot(screenPos, magic.xy)));
// }

// 32-sample Poisson disk
// const vec2 poissonDisk[32] = vec2[](
//     vec2(-0.9465, -0.1484), vec2(-0.7431,  0.5353),
//     vec2(-0.5863, -0.5879), vec2(-0.3935,  0.1025),
//     vec2(-0.2428,  0.7722), vec2(-0.1074, -0.3075),
//     vec2( 0.0542, -0.8645), vec2( 0.1267,  0.4300),
//     vec2( 0.2787, -0.1353), vec2( 0.3842,  0.6501),
//     vec2( 0.4714, -0.5537), vec2( 0.5765,  0.1675),
//     vec2( 0.6712, -0.3340), vec2( 0.7527,  0.4813),
//     vec2( 0.8745, -0.0910), vec2( 0.9601,  0.2637),
//     vec2(-0.8312,  0.3150), vec2(-0.6142, -0.2890),
//     vec2(-0.4581,  0.6310), vec2(-0.2134, -0.7520),
//     vec2(-0.0678,  0.4210), vec2( 0.1893, -0.5430),
//     vec2( 0.3215,  0.8140), vec2( 0.4890, -0.0230),
//     vec2( 0.6340,  0.3470), vec2( 0.7810, -0.5690),
//     vec2(-0.3467,  0.2560), vec2(-0.1290, -0.1120),
//     vec2( 0.0910,  0.1560), vec2( 0.2340, -0.3410),
//     vec2(-0.5120,  0.0420), vec2( 0.4210,  0.5120)
// );

// PCSS Blocker search
// float FindAverageBlockerDepthGL(vec2 uv, float receiverDepth, float searchRadius,
//                                  sampler2D depthSampler) {
//     float blockerSum = 0.0;
//     int blockerCount = 0;
//
//     float angle = InterleavedGradientNoise(gl_FragCoord.xy) * 6.283185;
//     float sa = sin(angle);
//     float ca = cos(angle);
//     mat2 rotation = mat2(ca, sa, -sa, ca);
//
//     for (int i = 0; i < 16; i++) {
//         vec2 offset = rotation * poissonDisk[i] * searchRadius;
//         float sampleDepth = texture(depthSampler, uv + offset).r;
//         if (sampleDepth < receiverDepth) {
//             blockerSum += sampleDepth;
//             blockerCount++;
//         }
//     }
//
//     if (blockerCount == 0)
//         return -1.0;
//
//     return blockerSum / float(blockerCount);
// }

// PCSS Variable-kernel PCF
// float PCSSFilterGL(vec2 uv, float receiverDepth, float filterRadius,
//                     sampler2DShadow shadowSampler) {
//     float angle = InterleavedGradientNoise(gl_FragCoord.xy) * 6.283185;
//     float sa = sin(angle);
//     float ca = cos(angle);
//     mat2 rotation = mat2(ca, sa, -sa, ca);
//
//     float shadow = 0.0;
//     for (int i = 0; i < 32; i++) {
//         vec2 offset = rotation * poissonDisk[i] * filterRadius;
//         shadow += texture(shadowSampler, vec3(uv + offset, receiverDepth));
//     }
//     return shadow / 32.0;
// }

// NOTE: The above functions are provided as commented-out reference
// for integration into the main PBR shader. The Vulkan backend
// (default_shader.frag) already has full PCSS implementation.
// For OpenGL, these functions should be added to default_shader_gl.frag
// when shadow mapping is enabled for the OpenGL backend.
