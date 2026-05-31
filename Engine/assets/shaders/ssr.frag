#version 450

// ============================================================
// Screen Space Reflections (SSR) — view-space ray march
//
// Reads:
//   set 0 binding 0: gNormalRough  (world-space normal encoded + roughness.a)
//   set 0 binding 1: gWorldPos     (world-space position, R32G32B32A32)
//   set 0 binding 2: gDepth        (depth buffer)
//   set 0 binding 3: gMetalEmit    (metallic.r + emissive.gba) - for F0
//   set 0 binding 4: gAlbedoAO     (albedo.rgb + AO.a) - for metallic F0
//   set 0 binding 5: sceneHDR      (lit HDR scene color)
//
// UBO (set 1 binding 0): SSRParams — view/proj matrices, ray march params
//
// Output: R16G16B16A16 premultiplied (rgb = sceneColor * weight, a = weight)
// ============================================================

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D gNormalRough;
layout(set = 0, binding = 1) uniform sampler2D gWorldPos;
layout(set = 0, binding = 2) uniform sampler2D gDepth;
layout(set = 0, binding = 3) uniform sampler2D gMetalEmit;
layout(set = 0, binding = 4) uniform sampler2D gAlbedoAO;
layout(set = 0, binding = 5) uniform sampler2D sceneHDR;

layout(set = 1, binding = 0) uniform SSRParams {
    mat4  View;
    mat4  Projection;
    vec4  CameraPos;           // xyz = world pos
    vec2  ScreenSize;          // width, height
    float MaxDistance;         // view-space ray march distance
    float Thickness;           // depth intersection thickness (view space)
    int   NumSteps;            // coarse steps
    int   NumBinarySteps;      // refinement steps
    float RoughnessThreshold;  // beyond this -> no reflections
    float _pad;
};

// Interleaved gradient noise — Jorge Jimenez. Cheap, nicely hashed, used to
// jitter the ray start so banding breaks up.
float InterleavedGradientNoise(vec2 screenPos) {
    vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
    return fract(magic.z * fract(dot(screenPos, magic.xy)));
}

// Project a view-space point to screen UV.
// GBuffer stores UV.y=0 at screen top (geometry shaders flip Y), so we
// negate Y when forming the sampling UV — same convention as ssao.frag.
vec3 ProjectToUVDepth(vec3 viewPos) {
    vec4 clip = Projection * vec4(viewPos, 1.0);
    vec3 ndc  = clip.xyz / max(clip.w, 1e-6);
    vec3 uv;
    uv.x = ndc.x * 0.5 + 0.5;
    uv.y = -ndc.y * 0.5 + 0.5;
    uv.z = ndc.z;  // depth in [0,1] after perspective divide
    return uv;
}

void main() {
    // Early-out: sky and out-of-range pixels contribute no reflection.
    float centerDepth = texture(gDepth, fragUV).r;
    if (centerDepth >= 0.9999) {
        outColor = vec4(0.0);
        return;
    }

    // GBuffer samples.
    vec4  normalRg = texture(gNormalRough, fragUV);
    vec3  worldN   = normalize(normalRg.rgb * 2.0 - 1.0);
    float roughness = normalRg.a;

    // Cheap exit for very rough surfaces — reflections would be incoherent
    // and dominated by IBL; skipping here also cuts the work.
    if (roughness >= RoughnessThreshold) {
        outColor = vec4(0.0);
        return;
    }

    vec3 worldPos = texture(gWorldPos, fragUV).xyz;

    // View + reflect in WORLD space, then transform to VIEW space for the march.
    vec3 V = normalize(CameraPos.xyz - worldPos);
    vec3 R = reflect(-V, worldN);

    vec3 viewPos = (View * vec4(worldPos, 1.0)).xyz;
    vec3 viewR   = normalize(mat3(View) * R);

    // Back-face cull: if the reflection points into the surface (or parallel),
    // there is no meaningful ray to march.
    float NdotV = max(dot(worldN, V), 0.0);

    // Small bias along the reflected direction to avoid self-intersection
    // at grazing angles. Scale with view-space depth so near surfaces don't
    // punch too-far starts and far surfaces still clear themselves.
    float startBias = max(0.01, abs(viewPos.z) * 0.001);
    vec3  rayStartVS = viewPos + viewR * startBias;

    // Total march distance in view space (capped by user param).
    float totalDist = MaxDistance;

    // Jitter the starting offset based on fragment position — breaks banding
    // the same way a blue-noise dither would, for essentially zero cost.
    float jitter = InterleavedGradientNoise(gl_FragCoord.xy);

    int numSteps = max(NumSteps, 1);
    float stepSize = totalDist / float(numSteps);

    // Hit state.
    float hitT            = -1.0;
    float lastSampleDepth = 0.0;
    vec3  hitUVDepth      = vec3(0.0);
    bool  skyHit          = false;  // true when the ray exits into the skybox

    for (int i = 1; i <= 64; ++i) {
        if (i > numSteps) break;

        float t = (float(i) + jitter - 0.5) * stepSize;
        vec3  rayVS = rayStartVS + viewR * t;

        if (rayVS.z > -0.001) continue;

        vec3 uvd = ProjectToUVDepth(rayVS);

        if (uvd.x < 0.0 || uvd.x > 1.0 || uvd.y < 0.0 || uvd.y > 1.0) continue;

        float sampleDepth = texture(gDepth, uvd.xy).r;

        // Sky pixel: the reflected ray exits geometry into the skybox.
        // Record the first sky hit and stop — sky is a valid reflection surface.
        if (sampleDepth >= 0.9999) {
            hitT      = t;
            hitUVDepth = uvd;
            skyHit    = true;
            break;
        }

        float delta = uvd.z - sampleDepth;
        if (delta > 0.0 && delta < Thickness) {
            hitT            = t;
            hitUVDepth      = uvd;
            lastSampleDepth = sampleDepth;
            break;
        }
    }

    if (hitT < 0.0) {
        outColor = vec4(0.0);
        return;
    }

    // Binary search refinement — skip for sky hits (no geometry to refine against).
    float lo = max(hitT - stepSize, 0.0);
    float hi = hitT;
    vec3  finalUVD = hitUVDepth;

    if (!skyHit) {
        int numBinary = max(NumBinarySteps, 0);
        for (int i = 0; i < 16; ++i) {
            if (i >= numBinary) break;

            float mid = 0.5 * (lo + hi);
            vec3  rayVS = rayStartVS + viewR * mid;
            vec3  uvd   = ProjectToUVDepth(rayVS);

            if (uvd.x < 0.0 || uvd.x > 1.0 || uvd.y < 0.0 || uvd.y > 1.0) {
                hi = mid; continue;
            }

            float sampleDepth = texture(gDepth, uvd.xy).r;
            float delta = uvd.z - sampleDepth;

            if (delta > 0.0 && delta < Thickness) {
                hi = mid;
                finalUVD = uvd;
                lastSampleDepth = sampleDepth;
            } else {
                lo = mid;
            }
        }
    }

    // Fetch PBR data needed for Fresnel weight.
    vec4 metalEmit = texture(gMetalEmit, fragUV);
    vec4 albedoAO  = texture(gAlbedoAO,  fragUV);
    vec3 albedo    = albedoAO.rgb;
    float metallic = metalEmit.r;

    // F0: 0.04 for dielectrics, albedo for metals.
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // Schlick Fresnel at view angle — use max(NdotV,0) to keep positive.
    vec3 fresnel = F0 + (1.0 - F0) * pow(max(1.0 - NdotV, 0.0), 5.0);

    // Roughness fade: reflections disappear as roughness approaches the
    // threshold. Map [0, RoughnessThreshold] -> [1, 0] smoothly.
    float roughFade = 1.0 - smoothstep(0.0, max(RoughnessThreshold, 1e-3), roughness);

    // Edge fade: soft falloff within 0.1 of any screen border.
    vec2 edgeDist = min(finalUVD.xy, 1.0 - finalUVD.xy);
    float edgeFade = clamp(min(edgeDist.x, edgeDist.y) * 10.0, 0.0, 1.0);

    // Hit confidence: how tightly the ray matched the depth buffer.
    // Sky hits are always confident — lastSampleDepth stays 0 for them so
    // the gap would be ~1.0 (NDC) which kills the weight; bypass it.
    float hitGap = skyHit ? 0.0 : max(abs(finalUVD.z - lastSampleDepth), 0.0);
    float hitConfidence = clamp(1.0 - hitGap / max(Thickness, 1e-5), 0.0, 1.0);

    // Distance fade — reflections fade out the further the ray travels
    // (far ray hits are noisier and we don't want them dominating).
    float distFade = clamp(1.0 - hitT / max(MaxDistance, 1e-3), 0.0, 1.0);

    // Combine all weights. Premultiply the colour so the composite step can
    // just ADD the reflection into the HDR scene.
    vec3 reflectedColor = texture(sceneHDR, finalUVD.xy).rgb;

    float weightScalar = roughFade * edgeFade * hitConfidence * distFade;
    vec3  weight       = fresnel * weightScalar;

    outColor = vec4(reflectedColor * weight, weightScalar);
}
