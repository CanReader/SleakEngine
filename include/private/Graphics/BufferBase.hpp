#ifndef _BUFFERBASE_HPP_
#define _BUFFERBASE_HPP_

#include "ResourceBase.hpp"
#include <Core/OSDef.hpp>
#include <cstring>

namespace Sleak {
    namespace RenderEngine {

        enum class BufferType { Vertex = 0, Index = 1, Constant = 2, ShaderResource = 3, DepthStencil = 4, RenderTarget = 5, UnorderedAccess = 6 };
        
        class ENGINE_API BufferBase : public ResourceBase {
        public:
            virtual bool Map() = 0;
            virtual void Unmap() = 0;
            virtual void Update() = 0;
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

            // CPU-side shadow copy for shadow pass (avoids GPU readback)
            const void* GetCPUShadowCopy() const { return m_cpuShadowCopy; }
            size_t GetCPUShadowCopySize() const { return m_cpuShadowCopySize; }
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
            // Small inline storage for transform CB shadow copy (128 bytes = 2 matrices)
            const void* m_cpuShadowCopy = nullptr;
            size_t m_cpuShadowCopySize = 0;
            alignas(16) char m_cpuShadowStorage[128] = {};
        };
    };    
};

#endif