#include "../../include/private/Graphics/ResourceManager.hpp"
#include "../../include/private/Graphics/BufferBase.hpp"
#include "../../include/private/Graphics/Vertex.hpp"
#include "../../include/private/Graphics/ConstantBuffer.hpp"
#include "../../include/private/Graphics/RenderCommandQueue.hpp"
#include <ECS/Components/MeshComponent.hpp>

namespace Sleak {
MeshComponent::MeshComponent(GameObject* object, MeshData data) : Component(object) {
        VertexBuffer = RefPtr(RenderEngine::ResourceManager::CreateBuffer(
            RenderEngine::BufferType::Vertex,
            data.vertices.GetSizeInBytes(),
            data.vertices.GetRawData()));
        
        IndexBuffer = RefPtr(RenderEngine::ResourceManager::CreateBuffer(
            RenderEngine::BufferType::Index,
            data.indices.GetByteSize(), 
            data.indices.GetRawData()));

            VertexCount = data.vertices.GetSize();
            IndexCount = data.indices.GetSize();
    }

    bool MeshComponent::Initialize() {
        if (!VertexBuffer.IsValid())
            return false;
        
        if (!IndexBuffer.IsValid())
            return false;
                
        bIsInitialized = true;

        return true;
    }

    void MeshComponent::Update(float deltaTime) {
        if (!bIsInitialized) 
            return;

        RenderEngine::RenderCommandQueue::GetInstance()->SubmitDrawIndexed(VertexBuffer,IndexBuffer,ConstantBuffers,IndexCount);
    }

    void MeshComponent::SetVertexBuffer(RefPtr<RenderEngine::BufferBase>& buffer) {
        this->VertexBuffer = buffer;
    }
    
    void MeshComponent::SetIndexBuffer(RefPtr<RenderEngine::BufferBase>& buffer) {
        this->IndexBuffer = buffer;
    }

    void MeshComponent::AddConstantBuffer(RenderEngine::TransformBuffer& bufferData) {
        auto buffer = RefPtr(RenderEngine::ResourceManager::CreateBuffer(
            RenderEngine::BufferType::Constant, sizeof(RenderEngine::TransformBuffer),
            &bufferData));

        ConstantBuffers.add(buffer);
    }

    void MeshComponent::AddConstantBuffer(
        RefPtr<RenderEngine::BufferBase>& buffer) {
        ConstantBuffers.add(buffer);
    }

    }