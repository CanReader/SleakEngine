#ifndef _MESHCOMPONENT_H_
#define _MESHCOMPONENT_H_

#include <ECS/Component.hpp>
#include <Utility/Container/List.hpp>
#include <Math/AABB.hpp>
#include <Memory/ObjectPtr.hpp>
#include <Memory/RefPtr.hpp>

namespace Sleak {
    class MeshData;
    struct VoxelMeshData;
    namespace RenderEngine {
        class TransformBuffer;
        class BufferBase;
    }  // namespace RenderEngine

    struct VoidData {
        uint16_t size;  
        void* data;
    };

    class ENGINE_API MeshComponent : public Component {
    public:
        MeshComponent(GameObject* object) : Component(object) {}
        MeshComponent(GameObject*, MeshData data);
        MeshComponent(GameObject*, VoxelMeshData data);

        virtual bool Initialize() override;
        
        virtual void Update(float deltaTime) override;

        void SetVertexBuffer(RefPtr<RenderEngine::BufferBase>& buffer);

        void SetIndexBuffer(RefPtr<RenderEngine::BufferBase>& buffer);

        void AddConstantBuffer(RenderEngine::TransformBuffer& bufferData);

        void AddConstantBuffer(RefPtr<RenderEngine::BufferBase>& buffer);

        // World-space bounds used for CPU frustum culling of this mesh.
        void SetCullBoundsWorld(const Math::AABB& bounds);
        // Local-space bounds, transformed by the owner each Update.
        void SetCullBoundsLocal(const Math::AABB& bounds);

    private:
        RefPtr<RenderEngine::BufferBase> VertexBuffer{};
        RefPtr<RenderEngine::BufferBase> IndexBuffer{};
        List<RefPtr<RenderEngine::BufferBase>> ConstantBuffers{};

       uint32_t VertexCount;
       uint32_t IndexCount;

       Math::AABB m_cullBounds{};
       bool m_hasCullBounds = false;
       Math::AABB m_localBounds{};
       bool m_hasLocalBounds = false;

    };
}

#endif
