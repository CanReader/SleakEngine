#ifndef _RENDERCOMMANDS_H
#define _RENDERCOMMANDS_H

#include <mutex>
#include <Memory/RefPtr.h>
#include <Utility/Container/List.hpp>
#include <Graphics/ConstantBuffer.hpp>
#include "BufferBase.hpp"
#include <Graphics/Renderer.hpp>

#define RENDER_COMMAND(Type) void Execute(RenderContext* context) override; \
                CommandType GetType() const override { return CommandType::Type;}

namespace Sleak {
    namespace RenderEngine {

        class BufferBase;
        class RenderContext;
        class Shader;
        class Texture;

        enum class CommandType {
            Draw = 0,
            DrawIndexed = 1,
            DrawInstanced = 2,
            UpdateConstantBuffer = 3,
            BindConstantBuffer = 4,
            SetRenderTarget = 5,
            ClearRenderTarget = 6,
            SetViewport = 7,
            SetShader = 8,
            SetTexture = 9,
            SetMode = 10,
            SetFace = 11,      
            SetBlendStae = 12,
            SetDepthStencilState = 13,
            SetRasterizerState = 14,
            CustomCommand = 15
        };

        class RenderCommandBase {
        public:
            virtual ~RenderCommandBase() = default;
            virtual void Execute(RenderContext* context) = 0;
            virtual CommandType GetType() const = 0;

            void SetOwnerObjectID(uint32_t id) { m_ownerObjectID = id; }
            uint32_t GetOwnerObjectID() const { return m_ownerObjectID; }

        private:
            uint32_t m_ownerObjectID = 0;
        };

        class DrawCommand : public RenderCommandBase {
            public:
                DrawCommand(
                    RefPtr<BufferBase> vertexBuffer,
                    List<RefPtr<BufferBase>> constantBuffers,
                    uint32_t vertexCount,
                    uint32_t startVertexLocation = 0
                );
                
                RENDER_COMMAND(Draw)

            private:
                RefPtr<BufferBase> m_vertexBuffer;
                List<RefPtr<BufferBase>> m_constantBuffers;
                uint32_t m_vertexCount;
                uint32_t m_startVertexLocation;
        };

        class DrawIndexedCommand : public RenderCommandBase {
        public:
            DrawIndexedCommand(RefPtr<BufferBase> vertexBuffer,
                 RefPtr<BufferBase> indexBuffer, 
                 List<RefPtr<BufferBase>> constantBuffers,
                 uint32_t indexCount,
                 uint32_t startIndexLocation = 0,
                 int32_t baseVertexLocation = 0
                );
                 
            RENDER_COMMAND(DrawIndexed)
            
        private:
            RefPtr<BufferBase> m_vertexBuffer;
            RefPtr<BufferBase> m_indexBuffer;
            List<RefPtr<BufferBase>> m_constantBuffers;
            uint32_t m_indexCount;
            uint32_t m_startIndexLocation;
            int32_t m_baseVertexLocation;
        };

        class UpdateConstantBufferCommand : public RenderCommandBase {
            public:
                UpdateConstantBufferCommand(RefPtr<BufferBase> buffer, void* Data, uint16_t Size);

                RENDER_COMMAND(UpdateConstantBuffer)

                void* GetData() const 
                { 
                    return Data;
                }

            private:
                RefPtr<BufferBase> constantBuffer;
                void* Data;
                uint16_t Size;
        };

        class BindConstantBufferCommand : public RenderCommandBase {
            public:
                BindConstantBufferCommand(RefPtr<BufferBase> buffer, int slot);

                RENDER_COMMAND(BindConstantBuffer)

            private:
                RefPtr<BufferBase> constantBuffer;
                int slot;
        };

        class BindTextureCommand : public RenderCommandBase {
            public:
            BindTextureCommand(RefPtr<Texture> texture);

                RENDER_COMMAND(SetTexture)

            private:
                RefPtr<Texture> texture;
        };

        class BindShaderCommand : public RenderCommandBase {
            public:
            BindShaderCommand(RefPtr<Shader> shader);

                RENDER_COMMAND(SetShader)

            private:
                RefPtr<Shader> shader;
        };

        class SetRenderModeCommand : public RenderCommandBase {
            public:
                SetRenderModeCommand(RenderMode mode);

                RENDER_COMMAND(SetMode)

            private:
                RenderMode mode;
        };

        class SetRenderFaceCommand : public RenderCommandBase {
            public:
                SetRenderFaceCommand(RenderFace face);

                RENDER_COMMAND(SetFace)

            private:
                RenderFace face;
        };

        class CustomCommand : public RenderCommandBase {
            public:
                using ExecuteFunction = std::function<void(RenderContext*)>;
                
                CustomCommand(ExecuteFunction executeFunction);
                
                void Execute(RenderContext* context) override;           

                CommandType GetType() const override { return CommandType::CustomCommand; }
            private:
                ExecuteFunction m_executeFunction;
            };
    }
}

#endif
