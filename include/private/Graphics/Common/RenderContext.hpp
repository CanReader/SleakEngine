#pragma once

#include <cstdint>
#include <string>
#include <Memory/RefPtr.hpp>
#include <Graphics/Common/BufferBase.hpp>
#include <Graphics/Common/Shader.hpp>
#include <Runtime/Texture.hpp>
#include <Runtime/VertexLayout.hpp>

namespace Sleak {
    class Material;
    namespace RenderEngine {

        /// Rasterizer fill style for a draw.
        enum class RenderMode  {
    None,
    Fill,
    Wireframe,
    Points,
    Outline,
};

/// Which triangle winding gets culled.
enum class RenderFace {
    None,
    Back,
    Front
};

/// Depth test comparison function.
enum class DepthCompare {
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Equal,
    NotEqual,
    Always,
    Never
};

        /**
            * @class RenderContext
            * @brief Abstract interface for high-level graphics command execution and resource management.
            * * The RenderContext acts as the primary bridge between the Sleak Engine's logic and the 
            * underlying Graphics API (Vulkan, DX11, etc.). It provides a unified set of commands for:
            * - **Command Dispatch:** Executing non-indexed, indexed, and instanced draw calls.
            * - **State Management:** Configuring the pipeline state (Viewports, Culling, Rasterization).
            * - **Resource Creation:** Serving as a Factory for API-specific buffers, shaders, and textures.
            * - **Resource Binding:** Mapping hardware buffers to specific pipeline slots.
            * * @note This is a pure virtual interface. Implementations (e.g., VulkanRenderContext) 
            * must handle the specific synchronization and driver-level requirements of their respective APIs.
        */
        class RenderContext {
        public:
            // Rendering commands
            /// Non-indexed draw from the currently bound vertex buffer.
            virtual void Draw(uint32_t vertexCount) = 0;
            /// Indexed draw from the currently bound vertex/index buffers.
            virtual void DrawIndexed(uint32_t indexCount) = 0;
            /// Non-indexed instanced draw.
            virtual void DrawInstance(uint32_t instanceCount, uint32_t vertexPerInstance) = 0;
            /// Indexed instanced draw.
            virtual void DrawIndexedInstance(uint32_t instanceCount, uint32_t indexPerInstance) = 0;

            // State management
            virtual void SetRenderFace(RenderFace face) = 0;
            virtual void SetRenderMode(RenderMode mode) = 0;
            /// Sets the active viewport rect and depth range in pixel/NDC units.
            virtual void SetViewport(float x, float y, float width, float height, float minDepth = 0.0f, float maxDepth = 1.0f) = 0;
            virtual void ClearRenderTarget(float r, float g, float b, float a) = 0;
            virtual void ClearDepthStencil(bool clearDepth, bool clearStencil, float depth, uint8_t stencil) = 0;

            // Depth/cull state (for skybox and other special rendering)
            virtual void SetDepthWrite(bool enabled) { (void)enabled; }
            virtual void SetDepthCompare(DepthCompare compare) { (void)compare; }
            virtual void SetCullEnabled(bool enabled) { (void)enabled; }
            virtual void BindTexture(RefPtr<Sleak::Texture> texture, uint32_t slot = 0) { (void)texture; (void)slot; }
            virtual void BindTextureRaw(Sleak::Texture* texture, uint32_t slot = 0) { (void)texture; (void)slot; }

            // Skybox pipeline support (Vulkan needs separate pipeline for cubemap)
            virtual void BeginSkyboxPass() {}
            virtual void EndSkyboxPass() {}

            // Bone buffer support (Vulkan needs UBO path — push constants too small)
            virtual void BindBoneBuffer(RefPtr<BufferBase> buffer) { (void)buffer; }

            // Skinned pipeline support (Vulkan needs separate pipeline for skinned shaders)
            virtual void BeginSkinnedPass() {}
            virtual void EndSkinnedPass() {}

            // Voxel pipeline support (compact 48-byte vertex layout for chunk meshes)
            virtual void BeginVoxelPass() {}
            virtual void EndVoxelPass() {}

            // Custom vertex format pipeline support (registered via VertexFormatRegistry)
            /// Binds the pipeline built for a registered vertex layout in the active render pass.
            virtual void BeginCustomFormatPass(VertexFormatHandle format) { (void)format; }
            /// Restores the pipeline that was active before the custom format draws.
            virtual void EndCustomFormatPass() {}

            // Debug line rendering pipeline support
            virtual void BeginDebugLinePass() {}
            virtual void EndDebugLinePass() {}

            // Shadow pass support
            virtual void BeginShadowPass() {}
            virtual void EndShadowPass() {}
            virtual bool IsShadowPassActive() const { return false; }

            // Deferred rendering support
            // Returns true when a geometry pass (GBuffer) is active this frame.
            virtual bool IsInGeometryPass() const { return false; }
            // Returns true when deferred lighting is enabled for this renderer.
            virtual bool IsDeferredEnabled() const { return false; }
            // Bind the GBuffer geometry-pass shader (overrides material shader in geometry pass).
            virtual void BindGBufferShader() {}
            // Bind all PBR material resources (textures + UBO) for the GBuffer geometry pass.
            // Replaces BindGBufferShader + BindTexturesAndCB for the deferred path.
            virtual void BindPBRMaterial(Sleak::Material* material) { (void)material; }
            // Transition from geometry pass → lighting pass → bind final RT.
            // Called by RenderCommandQueue after all opaque draws.
            virtual void ExecuteDeferredLightingPass() {}
            // Begin the forward transparent sub-pass (renders on top of deferred result).
            virtual void BeginForwardTransparentPass() {}
            // End the forward transparent sub-pass.
            virtual void EndForwardTransparentPass() {}
            // Update the per-frame deferred CB (InvViewProj + screen size + near/far).
            virtual void UpdateDeferredCB(const void* data, uint32_t size) { (void)data; (void)size; }

            // Buffer binding
            virtual void BindVertexBuffer(RefPtr<BufferBase> buffer, uint32_t slot = 0) = 0;
            virtual void BindIndexBuffer(RefPtr<BufferBase> buffer, uint32_t slot = 0) = 0;
            virtual void BindConstantBuffer(RefPtr<BufferBase> buffer, uint32_t slot = 0) = 0;

            // Resource creation
            /// Allocates a backend buffer, optionally seeded with initial data.
            virtual BufferBase* CreateBuffer(BufferType Type, uint32_t size, void* data) = 0;            
            /// Compiles a shader program from source or a source-file path.
            virtual Shader* CreateShader(const std::string& shaderSource) = 0;
            /// Loads a texture from disk.
            virtual Texture* CreateTexture(const std::string& TexturePath) = 0;
            /// Creates a texture from an in-memory pixel buffer.
            virtual Texture* CreateTextureFromData(uint32_t width, uint32_t height, void* data) = 0;

        };
    };
};
