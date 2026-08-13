#ifndef _MESHCOMPONENT_H_
#define _MESHCOMPONENT_H_

#include <ECS/Component.hpp>
#include <Utility/Container/List.hpp>
#include <Math/AABB.hpp>
#include <Memory/ObjectPtr.hpp>
#include <Memory/RefPtr.hpp>

namespace Sleak {
    class MeshData;
    namespace RenderEngine {
        class TransformBuffer;
        class BufferBase;
    }  // namespace RenderEngine

    /// Untyped payload blob, currently unused by any buffer path.
    struct VoidData {
        uint16_t size;
        void* data;
    };

    /// Holds the GPU vertex/index/constant buffers for a renderable mesh and
    /// submits a draw call each Update, subject to frustum culling.
    class ENGINE_API MeshComponent : public Component {
    public:
        MeshComponent(GameObject* object) : Component(object) {}
        /// Uploads data's vertex/index arrays to new GPU buffers.
        MeshComponent(GameObject*, MeshData data);

        /// Validates that vertex and index buffers were created successfully.
        virtual bool Initialize() override;

        /// Culls against cull bounds if set, then submits an indexed draw call.
        virtual void Update(float deltaTime) override;

        void SetVertexBuffer(RefPtr<RenderEngine::BufferBase>& buffer);

        void SetIndexBuffer(RefPtr<RenderEngine::BufferBase>& buffer);

        /// Creates a new constant buffer from bufferData and appends it to the draw's buffer list.
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
