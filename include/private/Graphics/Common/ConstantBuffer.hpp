#ifndef _CONSTANTBUFFER_H_
#define _CONSTANTBUFFER_H_

#include <Math/Color.hpp>
#include <Math/Matrix.hpp>
#include <cstddef>
#include <cstdint>

namespace Sleak {
    namespace RenderEngine {

        /// Well-known constant buffer slot identifiers.
        enum class CBIds {
            Transformation = 0,
            Material = 1,
            Lighting = 2
        };

        // Per-light GPU entry (64 bytes = 4 x 16-byte rows)
        /// One light's packed GPU representation, laid out for std140/cbuffer alignment.
        struct alignas(16) LightGPUEntry {
            // Row 0: Position.xyz + Type
            float PositionX, PositionY, PositionZ;
            uint32_t Type;  // 0=dir, 1=point, 2=spot, 3=area

            // Row 1: Direction.xyz + Intensity
            float DirectionX, DirectionY, DirectionZ;
            float Intensity;

            // Row 2: Color.rgb + Range
            float ColorR, ColorG, ColorB;
            float Range;

            // Row 3: SpotInnerCos + SpotOuterCos + AreaWidth + AreaHeight
            float SpotInnerCos;
            float SpotOuterCos;
            float AreaWidth;
            float AreaHeight;
        };

        // Full lighting constant buffer (64 + 64*16 + 32 trailing = 1120 bytes).
        // The trailing fog gradient + height-fog block is appended AFTER the
        // Lights[] array so that older shader UBO declarations (which stop at
        // Lights[]) still read a valid prefix. Don't reorder.
        static constexpr uint32_t MAX_LIGHTS = 16;

        /// Full per-frame lighting UBO: camera, ambient, fog, and the packed light array.
        struct alignas(16) LightCBData {
            // Header Row 0: CameraPos.xyz + NumActiveLights
            float CameraPosX, CameraPosY, CameraPosZ;
            uint32_t NumActiveLights;

            // Header Row 1: AmbientColor.rgb + AmbientIntensity
            float AmbientR, AmbientG, AmbientB;
            float AmbientIntensity;

            // Header Row 2: Fog color (HORIZON)
            float FogColorR, FogColorG, FogColorB, FogColorA;

            // Header Row 3: Fog distances + reserved
            float FogStart, FogEnd;
            float _reserved[2];

            // Per-light array
            LightGPUEntry Lights[MAX_LIGHTS];

            // ---- Trailing sky-matched + height fog block ----
            float FogColorZenith[4];   // ZENITH color of fog gradient
            float HeightFogTop;        // world Y above which height fog goes to 0
            float HeightFogDensity;    // max density at low elevations [0..1]
            float HeightFogFalloff;    // exponential falloff per world unit
            float HeightFogEnabled;    // 0 = off, 1 = on
        };

        /// Interface for a CPU-side payload that can be uploaded to a GPU constant buffer.
        struct ConstantBuffer {
        public:
            virtual void* GetData() const = 0;

            virtual uint16_t GetSize() const = 0;
        };

        /// Per-draw world-view-projection and world matrices.
        struct alignas(16) TransformBuffer : public ConstantBuffer {

            /// Composes WVP from world/view/proj and keeps World separately for lighting.
            TransformBuffer(const Sleak::Math::Matrix4& world,
                            const Sleak::Math::Matrix4& view,
                            const Sleak::Math::Matrix4& proj)
                : WVP(world * view * proj), World(world) {
            }

            /// Takes a pre-composed WVP with an identity world matrix (e.g. skybox, UI).
            explicit TransformBuffer(const Sleak::Math::Matrix4& mvp)
                : WVP(mvp), World(Sleak::Math::Matrix4::Identity()) {
            }

            virtual void* GetData() const override {
                return (void*) &WVP;
            }

            virtual uint16_t GetSize() const override {
                return sizeof(Sleak::Math::Matrix4) * 2;
            }

            private:
                Sleak::Math::Matrix4 WVP;
                Sleak::Math::Matrix4 World;
        };

        // GPU-aligned POD struct for material constant buffer data.
        // Total: 128 bytes (8 x 16-byte rows), matches all shader backends.
        /// Packed PBR material parameters uploaded to the material constant buffer.
        struct alignas(16) MaterialGPUData {
            // Row 0: Texture presence flags (16 bytes)
            uint32_t HasDiffuseMap;
            uint32_t HasNormalMap;
            uint32_t HasSpecularMap;
            uint32_t HasRoughnessMap;

            // Row 1: More texture flags (16 bytes)
            uint32_t HasMetallicMap;
            uint32_t HasAOMap;
            uint32_t HasEmissiveMap;
            uint32_t _pad0;

            // Row 2: Diffuse/Albedo color RGBA (16 bytes)
            float DiffuseR, DiffuseG, DiffuseB, DiffuseA;

            // Row 3: Specular color RGB + Shininess (16 bytes)
            float SpecularR, SpecularG, SpecularB;
            float Shininess;

            // Row 4: Emissive color RGB + Intensity (16 bytes)
            float EmissiveR, EmissiveG, EmissiveB;
            float EmissiveIntensity;

            // Row 5: PBR factors (16 bytes)
            float Metallic;
            float Roughness;
            float AO;
            float NormalIntensity;

            // Row 6: UV transform (16 bytes)
            float TilingX, TilingY;
            float OffsetX, OffsetY;

            // Row 7: Alpha properties (16 bytes)
            float Opacity;
            float AlphaCutoff;
            float _pad1, _pad2;
        };

        /// ConstantBuffer wrapper around MaterialGPUData.
        struct alignas(16) MaterialBuffer : public ConstantBuffer {

            MaterialBuffer() : gpuData{} {}

            MaterialBuffer(const MaterialGPUData& data)
                : gpuData(data) {}

            virtual void* GetData() const override {
                return (void*) &gpuData;
            }

            virtual uint16_t GetSize() const override {
                return sizeof(MaterialGPUData);
            }

            MaterialGPUData gpuData;
        };

        // GPU-aligned struct for light + shadow data, bound via UBO (set 2, binding 0)
        // NOTE: trailing fog/sky fields are appended in fixed order — do not
        // reorder. Shader UBO declarations may stop short of the full struct
        // (size mismatch in that direction is fine for std140/cbuffer reads).
        /// Shadow-pass UBO: light/shadow parameters, fog, and extra fill lights (set 2, binding 0).
        struct alignas(16) ShadowLightUBO {
            float LightDir[4];       // xyz + normalBias
            float LightColor[4];     // rgb + intensity
            float Ambient[4];        // rgb + intensity
            float CameraPos[4];      // xyz + sceneClock
            float LightVP[16];       // mat4 (light view-projection)
            float ShadowBias;
            float ShadowStrength;
            float ShadowTexelSize;
            float LightSize;         // world-space light size for PCSS penumbra
            float FogColor[4];       // rgb + a — HORIZON color of the fog gradient
            float FogStart;          // distance where fog begins
            float FogEnd;            // distance where fog fully obscures
            float _fogPad[2];
            float FogColorZenith[4]; // rgb + pad — ZENITH color of the fog gradient
            float HeightFogTop;      // world Y above which height fog density goes to 0
            float HeightFogDensity;  // max density at low elevations [0..1]
            float HeightFogFalloff;  // exponential falloff per world unit (e^-(top-y)*falloff)
            float HeightFogEnabled;  // 0 = off, 1 = on

            // Extra directional lights (fill, rim, etc.) — no shadow casting
            float ExtraLightDir[3][4];    // xyz + unused (w=0)
            float ExtraLightColor[3][4];  // rgb + intensity
            uint32_t NumExtraLights;      // 0-3
            float _extraPad[3];

            // NDC -> shadow clip, composed on CPU (InvViewProj * LightVP).
            // Shadow coords must use this — a per-fragment round trip
            // through reconstructed world position shimmers under rotation.
            float NdcToShadow[16];
        };
        static_assert(offsetof(ShadowLightUBO, LightDir) == 0);
        static_assert(offsetof(ShadowLightUBO, LightColor) == 16);
        static_assert(offsetof(ShadowLightUBO, Ambient) == 32);
        static_assert(offsetof(ShadowLightUBO, CameraPos) == 48);
        static_assert(offsetof(ShadowLightUBO, LightVP) == 64);
        static_assert(offsetof(ShadowLightUBO, ShadowBias) == 128);
        static_assert(offsetof(ShadowLightUBO, ShadowStrength) == 132);
        static_assert(offsetof(ShadowLightUBO, ShadowTexelSize) == 136);
        static_assert(offsetof(ShadowLightUBO, LightSize) == 140);
        static_assert(offsetof(ShadowLightUBO, FogColor) == 144);
        static_assert(offsetof(ShadowLightUBO, FogStart) == 160);
        static_assert(offsetof(ShadowLightUBO, FogEnd) == 164);
        static_assert(offsetof(ShadowLightUBO, _fogPad) == 168);
        static_assert(offsetof(ShadowLightUBO, FogColorZenith) == 176);
        static_assert(offsetof(ShadowLightUBO, HeightFogTop) == 192);
        static_assert(offsetof(ShadowLightUBO, HeightFogDensity) == 196);
        static_assert(offsetof(ShadowLightUBO, HeightFogFalloff) == 200);
        static_assert(offsetof(ShadowLightUBO, HeightFogEnabled) == 204);
        static_assert(offsetof(ShadowLightUBO, ExtraLightDir) == 208);
        static_assert(offsetof(ShadowLightUBO, ExtraLightColor) == 256);
        static_assert(offsetof(ShadowLightUBO, NumExtraLights) == 304);
        static_assert(offsetof(ShadowLightUBO, _extraPad) == 308);
        static_assert(offsetof(ShadowLightUBO, NdcToShadow) == 320);
        static_assert(sizeof(ShadowLightUBO) == 384);

        // GPU-aligned POD struct for post-process settings constant buffer.
        // Total: 16 bytes (1 x 16-byte row).
        /// Tonemapping/exposure settings uploaded to the post-process pass.
        struct alignas(16) PostProcessGPUData {
            float Exposure;        // HDR exposure multiplier (default 1.0)
            float Gamma;           // Gamma correction exponent (default 2.2)
            uint32_t TonemapEnabled; // 0 = passthrough, 1 = ACES tonemap
            uint32_t _ppPad0;
        };

        // GPU-aligned POD struct for SSAO settings constant buffer.
        // Total: 64 bytes (4 x 16-byte rows).
        static constexpr uint32_t SSAO_KERNEL_SIZE = 64;

        /// SSAO pass parameters: kernel size, radius/bias/power, and screen/near-far metrics.
        struct alignas(16) SSAOSettingsGPUData {
            // Row 0
            float Radius;          // Sample hemisphere radius (default 0.5)
            float Bias;            // Depth bias to prevent self-occlusion (default 0.025)
            float Power;           // AO power exponent (default 2.0)
            uint32_t KernelSize;   // Number of samples (default 64)

            // Row 1
            float ScreenWidth;
            float ScreenHeight;
            uint32_t SSAOEnabled;  // 0 = disabled, 1 = enabled
            uint32_t _ssaoPad0;

            // Row 2: near/far plane for linearizing depth
            float NearPlane;
            float FarPlane;
            float _ssaoPad1, _ssaoPad2;

            // Row 3: reserved
            float _ssaoPad3[4];
        };

        // GPU-aligned POD struct for PCSS shadow settings (used in PBR shader, DX11/OpenGL).
        // Total: 96 bytes (6 x 16-byte rows).
        /// PCSS soft-shadow parameters for the DX11/OpenGL PBR shader path.
        struct alignas(16) PCSSShadowGPUData {
            // Row 0-3: Light view-projection matrix (64 bytes)
            float LightVP[16];

            // Row 4: Shadow parameters
            float ShadowBias;
            float ShadowStrength;
            float ShadowTexelSize;
            float ShadowLightSize;

            // Row 5: Toggles
            uint32_t PCSSEnabled;
            uint32_t ShadowMapEnabled;
            float _shadowPad0, _shadowPad1;

            // Row 6-9: NDC -> shadow clip (see ShadowLightUBO::NdcToShadow)
            float NdcToShadow[16];
        };

        // GPU-aligned POD struct for SSAO composite settings (used in PBR shader).
        // Total: 16 bytes (1 x 16-byte row).
        /// Controls whether the forward PBR shader multiplies in the SSAO texture.
        struct alignas(16) SSAOCompositeGPUData {
            uint32_t SSAOCompositeEnabled; // 0 = disabled, 1 = multiply AO by SSAO texture
            float    ScreenWidth;
            float    ScreenHeight;
            float    _ssaoCompPad;
        };

        // GPU-aligned POD struct for IBL settings constant buffer.
        // Total: 16 bytes (1 x 16-byte row).
        /// Image-based lighting toggle, intensity, and prefiltered-map LOD range.
        struct alignas(16) IBLSettingsGPUData {
            uint32_t IBLEnabled;       // 0 = disabled, 1 = enabled
            float    IBLIntensity;     // multiplier for IBL contribution (default 1.0)
            float    MaxReflectionLOD; // max mip level for prefiltered env map (default 4.0)
            uint32_t _iblPad0;
        };

        /// ConstantBuffer wrapper around LightCBData.
        struct alignas(16) LightBuffer : public ConstantBuffer {
            LightBuffer() : gpuData{} {}
            LightBuffer(const LightCBData& data)
                : gpuData(data) {}

            virtual void* GetData() const override {
                return (void*)&gpuData;
            }

            virtual uint16_t GetSize() const override {
                return sizeof(LightCBData);
            }

            LightCBData gpuData;
        };

        // Per-frame CB for the deferred lighting pass (bound at slot 3).
        // Provides inverse view-projection for world-position reconstruction from depth.
        // Total: 80 bytes (5 x 16-byte rows).
        /// Per-frame deferred lighting pass CB: inverse view-projection plus screen/near-far metrics.
        struct alignas(16) DeferredCBData {
            // Rows 0-3: Inverse view-projection matrix (64 bytes)
            float InvViewProj[16];

            // Row 4: Screen size + near/far planes
            float ScreenWidth;
            float ScreenHeight;
            float NearPlane;
            float FarPlane;
        };

        /// ConstantBuffer wrapper around DeferredCBData.
        struct alignas(16) DeferredBuffer : public ConstantBuffer {
            DeferredBuffer() : gpuData{} {}
            DeferredBuffer(const DeferredCBData& data) : gpuData(data) {}

            virtual void* GetData() const override { return (void*)&gpuData; }
            virtual uint16_t GetSize() const override { return sizeof(DeferredCBData); }

            DeferredCBData gpuData;
        };

        /// Camera block for transparent formats that read the scene behind
        /// them (VertexLayoutDesc::transparentReadsScene), std140 layout.
        struct alignas(16) SceneReadUBO {
            float ViewProj[16];
            float InvViewProj[16];
            float ScreenWidth, ScreenHeight, InvScreenWidth, InvScreenHeight;
            float JitterX, JitterY, NearPlane, FarPlane;  // jitter in NDC
            float SceneValid, SkyValid, _pad0, _pad1;
        };
        static_assert(
            offsetof(SceneReadUBO, _pad1) + sizeof(float) ==
                sizeof(SceneReadUBO),
            "SceneReadUBO must match the std140 block without padding");

        /// Bumped with every FrameData append; Debug shaders compare it.
        inline constexpr uint32_t FRAME_DATA_VERSION = 1;
        /// Size of the opaque game block (set 2 binding 2, GL buffer 12).
        inline constexpr uint32_t FRAME_USER_DATA_MAX_BYTES = 1024;
        /// Reserved size of the cascade block (set 2 binding 3, GL buffer 13).
        inline constexpr uint32_t FRAME_CASCADE_BLOCK_BYTES = 1024;
        /// Reserved size of the atmosphere block (set 2 binding 4, GL
        /// buffer 15).
        inline constexpr uint32_t FRAME_ATMOSPHERE_BLOCK_BYTES = 1024;

        /// FrameData::Flags[0] bits, mirrored as SLEAK_FRAME_FLAG_* in GLSL.
        enum FrameFlagBits : uint32_t {
            FrameFlagSSAO = 1u << 0,
            FrameFlagTAA = 1u << 1,
            FrameFlagShadows = 1u << 2,
            FrameFlagDeferred = 1u << 3,
            FrameFlagGuard = 1u << 31,
        };

        /// Per-frame globals every pass can read (sleak/frame.glsl): Vulkan
        /// set 2 binding 1, GL uniform buffer 11. std140, append only.
        struct alignas(16) FrameData {
            uint32_t Version;
            uint32_t FrameIndex;
            float Time;
            float DeltaTime;
            float Screen[4];
            float Jitter[4];
            float CameraPos[4];
            float PrevCameraPos[4];
            float View[16];
            float Proj[16];
            float ViewProj[16];
            float InvViewProjRel[16];
            float PrevViewProjRel[16];
            float LightDir[4];
            float LightColor[4];
            float Ambient[4];
            uint32_t Flags[4];
        };
        static_assert(offsetof(FrameData, Version) == 0);
        static_assert(offsetof(FrameData, FrameIndex) == 4);
        static_assert(offsetof(FrameData, Time) == 8);
        static_assert(offsetof(FrameData, DeltaTime) == 12);
        static_assert(offsetof(FrameData, Screen) == 16);
        static_assert(offsetof(FrameData, Jitter) == 32);
        static_assert(offsetof(FrameData, CameraPos) == 48);
        static_assert(offsetof(FrameData, PrevCameraPos) == 64);
        static_assert(offsetof(FrameData, View) == 80);
        static_assert(offsetof(FrameData, Proj) == 144);
        static_assert(offsetof(FrameData, ViewProj) == 208);
        static_assert(offsetof(FrameData, InvViewProjRel) == 272);
        static_assert(offsetof(FrameData, PrevViewProjRel) == 336);
        static_assert(offsetof(FrameData, LightDir) == 400);
        static_assert(offsetof(FrameData, LightColor) == 416);
        static_assert(offsetof(FrameData, Ambient) == 432);
        static_assert(offsetof(FrameData, Flags) == 448);
        static_assert(sizeof(FrameData) == 464);
    }
}

#endif
