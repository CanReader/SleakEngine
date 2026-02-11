#ifndef _BUFFERBASE_HPP_
#define _BUFFERBASE_HPP_

#include "ResourceBase.hpp"
#include <Core/OSDef.hpp>

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
            
            inline void SetSlot(int slot) 
            { 
                Slot = slot;
            }

        protected:
            BufferType Type;
            size_t Size = 0;
            int Slot = 0;
            void* Data = nullptr;
            bool bIsMapped = false;        
        };
    };    
};

#endif