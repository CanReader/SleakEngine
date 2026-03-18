#ifndef _CONSTANTBUFFER_H_
#define _CONSTANTBUFFER_H_

#include <Math/Matrix.hpp>
#include <Math/Color.hpp>
#include <cstdint>

namespace Sleak {
    namespace RenderEngine {

        enum class CBIds {
            Transformation = 0,
            Material = 1,
            Lighting = 2
        };

        // Per-light GPU entry (64 bytes = 4 x 16-byte rows)
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

        // Full lighting constant buffer (64 + 64*16 = 1088 bytes)
        static constexpr uint32_t MAX_LIGHTS = 16;

        struct alignas(16) LightCBData {
            // Header Row 0: CameraPos.xyz + NumActiveLights
            float CameraPosX, CameraPosY, CameraPosZ;
            uint32_t NumActiveLights;

            // Header Row 1: AmbientColor.rgb + AmbientIntensity
            float AmbientR, AmbientG, AmbientB;
            float AmbientIntensity;

            // Header Row 2: Fog color
            float FogColorR, FogColorG, FogColorB, FogColorA;

            // Header Row 3: Fog distances + reserved
            float FogStart, FogEnd;
            float _reserved[2];

            // Per-light array
            LightGPUEntry Lights[MAX_LIGHTS];
        };

        struct ConstantBuffer {
        public:
            virtual void* GetData() const = 0;

            virtual uint16_t GetSize() const = 0;
        };

        struct alignas(16) TransformBuffer : public ConstantBuffer {

            TransformBuffer(const Sleak::Math::Matrix4& world,
                            const Sleak::Math::Matrix4& view,
                            const Sleak::Math::Matrix4& proj)
                : WVP(world * view * proj), World(world) {
            }

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
        struct alignas(16) ShadowLightUBO {
            float LightDir[4];       // xyz + pad
            float LightColor[4];     // rgb + intensity
            float Ambient[4];        // rgb + intensity
            float CameraPos[4];      // xyz + pad
            float LightVP[16];       // mat4 (light view-projection)
            float ShadowBias;
            float ShadowStrength;
            float ShadowTexelSize;
            float LightSize;         // world-space light size for PCSS penumbra
            float FogColor[4];       // rgb + alpha (matches sky color)
            float FogStart;          // distance where fog begins
            float FogEnd;            // distance where fog fully obscures
            float _fogPad[2];        // padding to 16-byte alignment
        };

        // GPU-aligned POD struct for post-process settings constant buffer.
        // Total: 16 bytes (1 x 16-byte row).
        struct alignas(16) PostProcessGPUData {
            float Exposure;        // HDR exposure multiplier (default 1.0)
            float Gamma;           // Gamma correction exponent (default 2.2)
            uint32_t TonemapEnabled; // 0 = passthrough, 1 = ACES tonemap
            uint32_t _ppPad0;
        };

        // GPU-aligned POD struct for SSAO settings constant buffer.
        // Total: 64 bytes (4 x 16-byte rows).
        static constexpr uint32_t SSAO_KERNEL_SIZE = 64;

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
        };

        // GPU-aligned POD struct for SSAO composite settings (used in PBR shader).
        // Total: 16 bytes (1 x 16-byte row).
        struct alignas(16) SSAOCompositeGPUData {
            uint32_t SSAOCompositeEnabled; // 0 = disabled, 1 = multiply AO by SSAO texture
            float    ScreenWidth;
            float    ScreenHeight;
            float    _ssaoCompPad;
        };

        // GPU-aligned POD struct for IBL settings constant buffer.
        // Total: 16 bytes (1 x 16-byte row).
        struct alignas(16) IBLSettingsGPUData {
            uint32_t IBLEnabled;       // 0 = disabled, 1 = enabled
            float    IBLIntensity;     // multiplier for IBL contribution (default 1.0)
            float    MaxReflectionLOD; // max mip level for prefiltered env map (default 4.0)
            uint32_t _iblPad0;
        };

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
    }
}

#endif
