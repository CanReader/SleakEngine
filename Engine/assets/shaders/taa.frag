#version 450

// ============================================================
// Temporal Anti-Aliasing (TAA) Resolve
//
// Reads:
//   set 0 binding 0: currentTex  — current HDR frame (jittered)
//   set 0 binding 1: historyTex  — previous resolved frame (ping-pong)
//   set 0 binding 2: gDepth      — depth buffer for reprojection
//
// UBO (set 1 binding 0): TAAParams
//
// GBuffer Y convention: UV.y=0 at screen top ↔ NDC.y = 1 - 2*uv.y
// (same convention as ssao.frag / ssr.frag)
// ============================================================

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D currentTex;
layout(set = 0, binding = 1) uniform sampler2D historyTex;
layout(set = 0, binding = 2) uniform sampler2D gDepth;

layout(set = 1, binding = 0) uniform TAAParams {
    mat4  InvCurrentVP;  // inverse of unjittered current view-projection
    mat4  PrevVP;        // previous frame unjittered view-projection
    vec2  ScreenSize;
    float BlendFactor;   // 0.1 = 10% current, 90% history (1.0 = disable history)
    float _pad;
};

void main() {
    vec2 texelSize = 1.0 / ScreenSize;

    vec3 current = texture(currentTex, fragUV).rgb;

    float depth = texture(gDepth, fragUV).r;
    if (depth >= 0.9999) {
        outColor = vec4(current, 1.0);
        return;
    }

    // Reconstruct world-space position.
    // GBuffer stores UV.y=0 at screen top, so NDC.y = 1 - 2*uv.y
    vec4 clipPos  = vec4(fragUV.x * 2.0 - 1.0, 1.0 - 2.0 * fragUV.y, depth, 1.0);
    vec4 world4   = InvCurrentVP * clipPos;
    vec3 worldPos = world4.xyz / world4.w;

    // Project to previous frame
    vec4 prevClip = PrevVP * vec4(worldPos, 1.0);
    vec3 prevNDC  = prevClip.xyz / prevClip.w;

    // Convert to UV (same GBuffer Y convention)
    vec2 prevUV;
    prevUV.x =  prevNDC.x * 0.5 + 0.5;
    prevUV.y = -prevNDC.y * 0.5 + 0.5;

    // Disocclusion / off-screen — use current frame only
    if (prevUV.x < 0.0 || prevUV.x > 1.0 ||
        prevUV.y < 0.0 || prevUV.y > 1.0) {
        outColor = vec4(current, 1.0);
        return;
    }

    // 3x3 neighborhood AABB to prevent ghosting.
    // History samples outside this range are clamped — they indicate
    // a surface that changed between frames (disocclusion, fast motion).
    vec3 nMin = vec3(1e10);
    vec3 nMax = vec3(-1e10);
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            vec3 s = texture(currentTex, fragUV + vec2(x, y) * texelSize).rgb;
            nMin = min(nMin, s);
            nMax = max(nMax, s);
        }
    }

    vec3 rawHistory = texture(historyTex, prevUV).rgb;
    vec3 history    = clamp(rawHistory, nMin, nMax);

    // Adaptive blend: when history was clamped hard (disocclusion / fast motion),
    // trust the current frame more. clampFactor approaches 1 when the raw history
    // was far outside the neighborhood AABB.
    float aabbRange   = max(length(nMax - nMin), 0.001);
    float clampDist   = length(rawHistory - history);
    // Cap at 0.5: even for fully-disoccluded pixels (clampFactor=1), keep
    // 50% clamped-history weight. Pure 100% current would let the sub-pixel
    // jitter shimmer through on animated character edges every frame.
    float clampFactor = clamp(clampDist / aabbRange * 2.0, 0.0, 1.0);
    float adaptBlend  = mix(BlendFactor, 0.5, clampFactor);

    vec3 resolved = mix(history, current, adaptBlend);
    outColor = vec4(resolved, 1.0);
}
