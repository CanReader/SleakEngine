#include <Runtime/Skybox.hpp>
#include <ECS/Components/MeshComponent.hpp>
#include <ECS/Components/TransformComponent.hpp>
#include <Runtime/InternalGeometry.hpp>
#include <Graphics/RenderCommandQueue.hpp>
#include <Camera/Camera.hpp>

namespace Sleak {   
    Skybox::Skybox(std::string name, float radius) : GameObject(name), radius(radius) {
        AddComponent<MeshComponent>(GetCubeMesh());
        AddComponent<TransformComponent>(Vector3D(0,0,0));

    }
    
    void Skybox::Initialize() {
        GameObject::Initialize();

        auto transform = GetComponent<TransformComponent>();
        if(transform) {
            transform->Scale(radius);
        }
    }

    void Skybox::Update(float DeltaTime) {
        GameObject::Update(DeltaTime);
        auto transform = GetComponent<TransformComponent>();
        if (transform) {
            
        }
    }
    
}