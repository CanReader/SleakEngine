#version 450

// ============================================================
// Screen Space Ambient Occlusion (HBAO-quality hemisphere AO)
//
// Reads:
//   set 0 binding 0: gNormalRough  (world-space normal encoded + roughness.a)
//   set 0 binding 1: gDepth        (depth buffer — world pos reconstructed)
//   set 0 binding 2: noiseTexture  (4x4 random unit vectors, tiled)
//
// UBO (set 1 binding 0): SSAOParams — kernel, view/proj/invViewProj matrices
//
// Output: single-channel R8 occlusion factor (1.0 = clear, 0.0 = occluded)
// ============================================================

layout(location = 0) in vec2 fragUV;
layout(location = 0) out float outOcclusion;

layout(set = 0, binding = 0) uniform sampler2D gNormalRough;
layout(set = 0, binding = 1) uniform sampler2D gDepth;
layout(set = 0, binding = 2) uniform sampler2D noiseTex;

layout(set = 1, binding = 0) uniform SSAOParams {
    mat4 View;           // world -> view
    mat4 Projection;     // view  -> clip
    mat4 InvViewProj;    // clip  -> world (depth reconstruction)
    vec4 Kernel[32];     // hemisphere samples in tangent space (xyz=dir, w=unused)
    vec2 ScreenSize;     // full-res width, height (for noise tiling)
    vec2 NoiseScale;     // ScreenSize / 4 for noise UV
    float Radius;        // world-space sample radius (metres)
    float Bias;           // depth comparison bias
    float Power;          // occlusion curve power
    float Intensity;      // overall darkness multiplier
    uint  KernelSize;     // number of samples to take (<= 32)
    float _pad0, _pad1, _pad2;
};

// Reconstruct world position from depth. Vulkan: geometry is Y-flipped in the
// GBuffer vertex shader and depth is [0,1], so NDC.y is negated vs the UV.
vec3 ReconstructWorldPos(vec2 uv, float depth) {
    vec4 ndc   = vec4(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y, depth, 1.0);
    vec4 world = InvViewProj * ndc;
    return world.xyz / world.w;
}

void main() {
    // Skip sky pixels — sky should read 1.0 (no occlusion).
    float depth = texture(gDepth, fragUV).r;
    if (depth >= 0.9999) {
        outOcclusion = 1.0;
        return;
    }

    // Reconstruct world-space position from depth; normal from GBuffer.
    vec3 worldPos = ReconstructWorldPos(fragUV, depth);
    vec3 worldN   = normalize(texture(gNormalRough, fragUV).rgb * 2.0 - 1.0);

    // Move into view space — AO is cleanest in the space where the
    // projection happens, and Z is monotonic along the view ray.
    vec3 viewPos = (View * vec4(worldPos, 1.0)).xyz;
    vec3 viewN   = normalize(mat3(View) * worldN);

    // Random rotation vector (in the tangent plane) — tiled from the 4x4 noise.
    vec3 randomVec = normalize(texture(noiseTex, fragUV * NoiseScale).xyz * 2.0 - 1.0);

    // Build a TBN centered at the normal. Gram-Schmidt: orthogonalise
    // randomVec against the normal, then bitangent = N x T.
    vec3 tangent   = normalize(randomVec - viewN * dot(randomVec, viewN));
    vec3 bitangent = cross(viewN, tangent);
    mat3 TBN       = mat3(tangent, bitangent, viewN);

    float occlusion = 0.0;
    uint kSize = max(KernelSize, 1u);
    for (uint i = 0u; i < kSize; ++i) {
        // Transform hemisphere sample from tangent space to view space.
        vec3 samplePos = viewPos + TBN * Kernel[i].xyz * Radius;

        // Project sample to clip space, then NDC, then UV.
        vec4 offset = Projection * vec4(samplePos, 1.0);
        offset.xyz /= offset.w;
        // The GBuffer geometry shaders apply gl_Position.y = -gl_Position.y
        // so the GBuffer textures have UV.y=0 at the TOP of the screen.
        // The projection matrix is standard (no Y-flip baked in), so its
        // clip-space Y=+1 maps to the TOP in OpenGL convention — but in the
        // stored GBuffer texture that is UV.y=0. We must negate Y here so
        // the GBuffer sample UV matches the GBuffer's storage layout.
        vec2 sampleUV;
        sampleUV.x =  offset.x * 0.5 + 0.5;
        sampleUV.y = -offset.y * 0.5 + 0.5;

        // Bounds check — any sample outside the screen contributes no occlusion.
        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 ||
            sampleUV.y < 0.0 || sampleUV.y > 1.0) continue;

        // Reconstruct the world-space position at this screen location and
        // transform into view space for depth comparison.
        float sampleDepth   = texture(gDepth, sampleUV).r;
        if (sampleDepth >= 0.9999) continue;  // sky — no occluder

        vec3 sampleWorld    = ReconstructWorldPos(sampleUV, sampleDepth);
        vec3 sampleViewPos  = (View * vec4(sampleWorld, 1.0)).xyz;

        // Range check — sample contributes less when the depth difference
        // between the frag and the occluder is much larger than the radius.
        // (avoids haloing around silhouettes)
        float rangeCheck = smoothstep(0.0, 1.0,
            Radius / max(abs(viewPos.z - sampleViewPos.z), 0.0001));

        // In view space the camera looks down -Z — "closer to the camera"
        // means a LESS negative Z (i.e. larger value). The occluder blocks
        // the sample when sampleViewPos.z >= samplePos.z + bias.
        occlusion += (sampleViewPos.z >= samplePos.z + Bias ? 1.0 : 0.0) * rangeCheck;
    }

    float ao = 1.0 - (occlusion / float(kSize)) * Intensity;
    outOcclusion = pow(clamp(ao, 0.0, 1.0), Power);
}
