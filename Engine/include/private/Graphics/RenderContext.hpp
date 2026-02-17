#pragma once

#include <cstdint>
#include <string>
#include <Memory/RefPtr.h>
#include <Graphics/BufferBase.hpp>
#include <Graphics/Shader.hpp>
#include <Runtime/Texture.hpp>

namespace Sleak {
    namespace RenderEngine {

        enum class RenderMode  {
    None,
    Fill,
    Wireframe,
    Points,
    Outline,
};

enum class RenderFace {
    None,
    Back,
    Front
};

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
            virtual void Draw(uint32_t vertexCount) = 0;
            virtual void DrawIndexed(uint32_t indexCount) = 0;
            virtual void DrawInstance(uint32_t instanceCount, uint32_t vertexPerInstance) = 0;
            virtual void DrawIndexedInstance(uint32_t instanceCount, uint32_t indexPerInstance) = 0;

            // State management
            virtual void SetRenderFace(RenderFace face) = 0;
            virtual void SetRenderMode(RenderMode mode) = 0;
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

            // Buffer binding
            virtual void BindVertexBuffer(RefPtr<BufferBase> buffer, uint32_t slot = 0) = 0;
            virtual void BindIndexBuffer(RefPtr<BufferBase> buffer, uint32_t slot = 0) = 0;
            virtual void BindConstantBuffer(RefPtr<BufferBase> buffer, uint32_t slot = 0) = 0;

            // Resource creation
            virtual BufferBase* CreateBuffer(BufferType Type, uint32_t size, void* data) = 0;            
            virtual Shader* CreateShader(const std::string& shaderSource) = 0;
            virtual Texture* CreateTexture(const std::string& TexturePath) = 0;
            virtual Texture* CreateTextureFromData(uint32_t width, uint32_t height, void* data) = 0;

        };
    };
};
