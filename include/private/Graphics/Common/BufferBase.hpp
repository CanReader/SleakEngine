#ifndef _BUFFERBASE_HPP_
#define _BUFFERBASE_HPP_

#include "ResourceBase.hpp"
#include <Core/OSDef.hpp>
#include <cstdint>
#include <cstring>

namespace Sleak {
    namespace RenderEngine {

        /// GPU buffer usage kind, drives backend binding flags and layout.
        enum class BufferType { Vertex = 0, Index = 1, Constant = 2, ShaderResource = 3, DepthStencil = 4, RenderTarget = 5, UnorderedAccess = 6 };
        
        /// Backend-agnostic GPU buffer: vertex, index, constant, or resource view target.
        class ENGINE_API BufferBase : public ResourceBase {
        public:
            /// Maps the buffer for CPU writes; returns false if already mapped or unmappable.
            virtual bool Map() = 0;
            virtual void Unmap() = 0;
            virtual void Update() = 0;
            /// Overwrites the buffer contents with new data of a given size.
            virtual void Update(void* data, size_t size) = 0;
            
            virtual void* GetData() = 0;

            inline size_t GetSize() { 
                return Size;
            }

            inline BufferType GetType() { return Type; }
            
            inline int GetSlot() const { return Slot; }

            inline void SetSlot(int slot)
            {
                Slot = slot;
            }

            // Mark as compact voxel vertex buffer (48-byte stride)
            bool IsVoxelFormat() const { return m_isVoxelFormat; }
            void SetVoxelFormat(bool v) { m_isVoxelFormat = v; }

            /// Registered custom vertex layout of this buffer; 0 means the engine default Vertex.
            uint32_t GetVertexFormat() const { return m_vertexFormat; }
            /// Tags the buffer with a VertexFormatRegistry handle so backends pick the matching pipeline.
            void SetVertexFormat(uint32_t handle) { m_vertexFormat = handle; }

            // CPU-side shadow copy for shadow pass (avoids GPU readback)
            const void* GetCPUShadowCopy() const { return m_cpuShadowCopy; }
            size_t GetCPUShadowCopySize() const { return m_cpuShadowCopySize; }
            /// Copies up to 128 bytes into the inline shadow-copy storage, clamping oversized input.
            void StoreCPUShadowCopy(const void* data, size_t size) {
                if (size > sizeof(m_cpuShadowStorage)) size = sizeof(m_cpuShadowStorage);
                memcpy(m_cpuShadowStorage, data, size);
                m_cpuShadowCopy = m_cpuShadowStorage;
                m_cpuShadowCopySize = size;
            }

        protected:
            BufferType Type;
            size_t Size = 0;
            int Slot = 0;
            void* Data = nullptr;
            bool bIsMapped = false;
            bool m_isVoxelFormat = false;
            uint32_t m_vertexFormat = 0;
            // Small inline storage for transform CB shadow copy (128 bytes = 2 matrices)
            const void* m_cpuShadowCopy = nullptr;
            size_t m_cpuShadowCopySize = 0;
            alignas(16) char m_cpuShadowStorage[128] = {};
        };
    };    
};

#endif