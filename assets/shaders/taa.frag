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

// Catmull-Rom history resample (Karis 5-tap): repeated bilinear resampling
// of the history buffer low-passes the whole image — this is what made the
// frame progressively blurry under camera motion.
vec3 SampleHistoryCatmullRom(vec2 uv) {
    vec2 samplePos = uv * ScreenSize;
    vec2 texPos1   = floor(samplePos - 0.5) + 0.5;
    vec2 f         = samplePos - texPos1;

    vec2 w0  = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1  = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2  = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3  = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;

    vec2 tc0  = (texPos1 - 1.0)            / ScreenSize;
    vec2 tc3  = (texPos1 + 2.0)            / ScreenSize;
    vec2 tc12 = (texPos1 + w2 / w12)       / ScreenSize;

    vec3  result =
        texture(historyTex, vec2(tc12.x, tc0.y )).rgb * (w12.x * w0.y ) +
        texture(historyTex, vec2(tc0.x,  tc12.y)).rgb * (w0.x  * w12.y) +
        texture(historyTex, vec2(tc12.x, tc12.y)).rgb * (w12.x * w12.y) +
        texture(historyTex, vec2(tc3.x,  tc12.y)).rgb * (w3.x  * w12.y) +
        texture(historyTex, vec2(tc12.x, tc3.y )).rgb * (w12.x * w3.y );
    float wsum = (w12.x * w0.y) + (w0.x * w12.y) + (w12.x * w12.y) +
                 (w3.x * w12.y) + (w12.x * w3.y);
    return max(result / wsum, vec3(0.0));
}

void main() {
    vec2 texelSize = 1.0 / ScreenSize;

    vec3 current = texture(currentTex, fragUV).rgb;

    ivec2 ip     = ivec2(fragUV * ScreenSize);
    ivec2 maxIp  = ivec2(ScreenSize) - 1;
    float depth  = texelFetch(gDepth, clamp(ip, ivec2(0), maxIp), 0).r;
    if (depth >= 0.9999) {
        outColor = vec4(current, 1.0);
        return;
    }

    // Closest-depth dilation with UNFILTERED depth reads. Bilinear-filtered
    // depth at silhouettes yields positions on neither surface — the source
    // of edge shimmer at distance where every texel is a silhouette.
    vec2  closestOff   = vec2(0.0);
    float closestDepth = depth;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float d = texelFetch(gDepth,
                                 clamp(ip + ivec2(x, y), ivec2(0), maxIp),
                                 0).r;
            if (d < closestDepth) {
                closestDepth = d;
                closestOff   = vec2(x, y);
            }
        }
    }

    // Reproject the closest surface in the neighborhood and apply its motion
    // to this pixel — edges follow the foreground surface.
    // GBuffer stores UV.y=0 at screen top, so NDC.y = 1 - 2*uv.y
    vec2 dilatedUV = fragUV + closestOff * texelSize;
    vec4 clipPos  = vec4(dilatedUV.x * 2.0 - 1.0, 1.0 - 2.0 * dilatedUV.y,
                         closestDepth, 1.0);
    vec4 world4   = InvCurrentVP * clipPos;
    vec3 worldPos = world4.xyz / world4.w;

    vec4 prevClip = PrevVP * vec4(worldPos, 1.0);
    vec3 prevNDC  = prevClip.xyz / prevClip.w;

    vec2 prevUVd;
    prevUVd.x =  prevNDC.x * 0.5 + 0.5;
    prevUVd.y = -prevNDC.y * 0.5 + 0.5;
    vec2 velocity = dilatedUV - prevUVd;
    vec2 prevUV   = fragUV - velocity;

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

    vec3 rawHistory = SampleHistoryCatmullRom(prevUV);
    vec3 history    = clamp(rawHistory, nMin, nMax);

    // Adaptive blend: when history was clamped hard (disocclusion / fast motion),
    // trust the current frame more. clampFactor approaches 1 when the raw history
    // was far outside the neighborhood AABB. 0.5 floor: pure 100% current would
    // let sub-pixel jitter shimmer through on hard-clamped pixels every frame.
    // (Was 0.35 to hide shadow-edge shimmer — that is now handled by the wider
    //  PCF, and 0.35 smeared distant sub-pixel detail that clamps every frame.)
    float aabbRange   = max(length(nMax - nMin), 0.001);
    float clampDist   = length(rawHistory - history);
    float clampFactor = clamp(clampDist / aabbRange * 2.0, 0.0, 1.0);
    float adaptBlend  = mix(BlendFactor, 0.5, clampFactor);

    vec3 resolved = mix(history, current, adaptBlend);
    outColor = vec4(resolved, 1.0);
}
