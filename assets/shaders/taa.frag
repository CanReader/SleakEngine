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
    mat4  CurrentToPrevClip;  // inverse(current VP) * previous VP, unjittered
    vec2  ScreenSize;
    float BlendFactor;   // current-frame weight when still (1.0 = history reset)
    float _pad;
};

//  NOTE: Do not change this. Global constant for standard sRGB/BT.709 human perception weights
const vec3 LUMA_WEIGHTS = vec3(0.2126, 0.7152, 0.0722);

const float SKY_DEPTH             = 0.9999;
// Variance box half-size in standard deviations: lower = sharper, more flicker
const float VARIANCE_GAMMA        = 1.0;
// Current-frame weight once a pixel moves MOTION_PX_FOR_MOVING pixels per frame
const float BLEND_MOVING          = 0.2;
// Current-frame weight on fast-moving reactive pixels (transparent draws zero
// the scene alpha). Refraction/reflection parallax grows with camera motion,
// so a still or slow view keeps full history and steady glints.
const float BLEND_REACTIVE        = 0.5;
const float MOTION_PX_FOR_MOVING  = 2.0;
const float NEIGHBOURHOOD_SIZE    = 9.0;
const float TONEMAP_INVERSE_FLOOR = 1e-3;
const float CLIP_EPSILON          = 1e-5;

float Luma(vec3 color) { return dot(color, LUMA_WEIGHTS); }

// Karis weighting: blend in a compressed range so bright sky cannot dominate
// the average at leaf/sky edges.
vec3 Tonemap(vec3 color) { return color / (1.0 + Luma(color)); }
vec3 TonemapInverse(vec3 color) {
    return color / max(1.0 - Luma(color), TONEMAP_INVERSE_FLOOR);
}

vec3 RGBToYCoCg(vec3 c) {
    return vec3( 0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                 0.5  * c.r              - 0.5  * c.b,
                -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
vec3 YCoCgToRGB(vec3 c) {
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

vec3 FetchTonemappedYCoCg(ivec2 p, ivec2 maxIp) {
    vec3 c = texelFetch(currentTex, clamp(p, ivec2(0), maxIp), 0).rgb;
    return RGBToYCoCg(Tonemap(c));
}

// Pulls hist toward the box center until it touches the box. Unlike a
// per-channel clamp this never invents a colour that was not there.
vec3 ClipToAABB(vec3 hist, vec3 boxMin, vec3 boxMax) {
    vec3  center  = 0.5 * (boxMax + boxMin);
    vec3  extents = max(0.5 * (boxMax - boxMin), vec3(CLIP_EPSILON));
    vec3  d       = hist - center;
    vec3  u       = abs(d / extents);
    float m       = max(u.x, max(u.y, u.z));
    return m > 1.0 ? center + d / m : hist;
}

// Catmull-Rom history resample (Karis 5-tap): repeated bilinear resampling
// of the history buffer low-passes the whole image under camera motion.
vec3 SampleHistoryCatmullRom(vec2 uv) {
    vec2 samplePos = uv * ScreenSize;
    vec2 texPos1   = floor(samplePos - 0.5) + 0.5;
    vec2 f         = samplePos - texPos1;

    vec2 w0  = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1  = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2  = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3  = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;

    vec2 tc0  = (texPos1 - 1.0)      / ScreenSize;
    vec2 tc3  = (texPos1 + 2.0)      / ScreenSize;
    vec2 tc12 = (texPos1 + w2 / w12) / ScreenSize;

    vec3  result =
        textureLod(historyTex, vec2(tc12.x, tc0.y ), 0.0).rgb * (w12.x * w0.y ) +
        textureLod(historyTex, vec2(tc0.x,  tc12.y), 0.0).rgb * (w0.x  * w12.y) +
        textureLod(historyTex, vec2(tc12.x, tc12.y), 0.0).rgb * (w12.x * w12.y) +
        textureLod(historyTex, vec2(tc3.x,  tc12.y), 0.0).rgb * (w3.x  * w12.y) +
        textureLod(historyTex, vec2(tc12.x, tc3.y ), 0.0).rgb * (w12.x * w3.y );
    float wsum = (w12.x * w0.y) + (w0.x * w12.y) + (w12.x * w12.y) +
                 (w3.x * w12.y) + (w12.x * w3.y);
    return max(result / wsum, vec3(0.0));
}

void main() {
    vec2  texelSize = 1.0 / ScreenSize;
    ivec2 ip        = ivec2(fragUV * ScreenSize);
    ivec2 maxIp     = ivec2(ScreenSize) - 1;
    vec4  currentA  = texelFetch(currentTex, clamp(ip, ivec2(0), maxIp), 0);
    vec3  current   = currentA.rgb;
    float reactive  = 1.0 - clamp(currentA.a, 0.0, 1.0);

    // 3x3 depth via gathers around the corner shared with the up-left texel
    vec2 cuv = vec2(ip) * texelSize;
    vec4 g00 = textureGather(gDepth, cuv);
    vec4 g10 = textureGatherOffset(gDepth, cuv, ivec2(1, 0));
    vec4 g01 = textureGatherOffset(gDepth, cuv, ivec2(0, 1));
    vec4 g11 = textureGatherOffset(gDepth, cuv, ivec2(1, 1));

    // Closest-depth dilation with unfiltered depth: edges follow the
    // foreground surface, including sky pixels that border a leaf.
    vec2  closestOff   = vec2(0.0);
    float closestDepth = g00.y;
    if (g00.w < closestDepth) { closestDepth = g00.w; closestOff = vec2(-1.0, -1.0); }
    if (g00.x < closestDepth) { closestDepth = g00.x; closestOff = vec2(-1.0,  0.0); }
    if (g01.x < closestDepth) { closestDepth = g01.x; closestOff = vec2(-1.0,  1.0); }
    if (g00.z < closestDepth) { closestDepth = g00.z; closestOff = vec2( 0.0, -1.0); }
    if (g01.y < closestDepth) { closestDepth = g01.y; closestOff = vec2( 0.0,  1.0); }
    if (g10.z < closestDepth) { closestDepth = g10.z; closestOff = vec2( 1.0, -1.0); }
    if (g10.y < closestDepth) { closestDepth = g10.y; closestOff = vec2( 1.0,  0.0); }
    if (g11.y < closestDepth) { closestDepth = g11.y; closestOff = vec2( 1.0,  1.0); }

    // Whole neighbourhood is sky: nothing to reproject
    if (closestDepth >= SKY_DEPTH) {
        outColor = vec4(current, 1.0);
        return;
    }

    // Reproject the closest surface with the previous frame's camera
    vec2 dilatedUV = fragUV + closestOff * texelSize;
    vec4 clipPos   = vec4(dilatedUV.x * 2.0 - 1.0, 1.0 - 2.0 * dilatedUV.y,
                          closestDepth, 1.0);
    vec4 prevClip  = CurrentToPrevClip * clipPos;
    vec3 prevNDC   = prevClip.xyz / prevClip.w;
    vec2 prevUVd  = vec2(prevNDC.x * 0.5 + 0.5, -prevNDC.y * 0.5 + 0.5);
    vec2 velocity = dilatedUV - prevUVd;
    vec2 prevUV   = fragUV - velocity;

    // Came from off-screen: no history exists
    if (any(lessThan(prevUV, vec2(0.0))) || any(greaterThan(prevUV, vec2(1.0)))) {
        outColor = vec4(current, 1.0);
        return;
    }

    // Neighbourhood statistics in tonemapped YCoCg
    vec3 currentYC = vec3(0.0);
    vec3 m1        = vec3(0.0);
    vec3 m2        = vec3(0.0);
    vec3 nMin      = vec3(1e10);
    vec3 nMax      = vec3(-1e10);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec3 s = FetchTonemappedYCoCg(ip + ivec2(x, y), maxIp);
            if (x == 0 && y == 0) currentYC = s;
            m1  += s;
            m2  += s * s;
            nMin = min(nMin, s);
            nMax = max(nMax, s);
        }
    }
    vec3 mean  = m1 / NEIGHBOURHOOD_SIZE;
    vec3 sigma = sqrt(max(m2 / NEIGHBOURHOOD_SIZE - mean * mean, vec3(0.0)));

    // Variance box, never wider than the real min/max of the neighbourhood
    vec3 boxMin = max(nMin, mean - VARIANCE_GAMMA * sigma);
    vec3 boxMax = min(nMax, mean + VARIANCE_GAMMA * sigma);

    vec3 historyYC = RGBToYCoCg(Tonemap(SampleHistoryCatmullRom(prevUV)));
    historyYC      = ClipToAABB(historyYC, boxMin, boxMax);

    // Still: lean on history for a converged, stable image.
    // Moving: take more of the current frame so reprojection blur cannot build up.
    float motionPx    = length(velocity * ScreenSize);
    float motion      = clamp(motionPx / MOTION_PX_FOR_MOVING, 0.0, 1.0);
    float motionBlend = mix(BlendFactor, max(BlendFactor, BLEND_MOVING), motion);
    motionBlend = max(motionBlend, reactive * motion * BLEND_REACTIVE);

    vec3 resolvedYC = mix(historyYC, currentYC, motionBlend);
    vec3 resolved   = max(YCoCgToRGB(resolvedYC), vec3(0.0));
    outColor = vec4(TonemapInverse(resolved), 1.0);
}
