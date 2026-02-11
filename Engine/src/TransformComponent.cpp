#include <ECS/Components/TransformComponent.hpp>
#include <Graphics/BufferBase.hpp>
#include <Graphics/ConstantBuffer.hpp>
#include <Graphics/ResourceManager.hpp>
#include <Camera/Camera.hpp>
#include <Graphics/RenderCommandQueue.hpp>
#include <ECS/Components/MeshComponent.hpp>

namespace Sleak {

    // Constructors
    TransformComponent::TransformComponent(GameObject* object, const Vector3D& position) : Component(object), position(position) {}

    TransformComponent::TransformComponent(GameObject* object,
                                           const Vector3D& position,
                                           const Quaternion& rotation)
        : Component(object), position(position), rotation(rotation) {}

    TransformComponent::TransformComponent(GameObject* object,
                                           const Vector3D& position,
                                           const Quaternion& rotation,
                                           const Vector3D& scale)
        : Component(object),
          position(position),
          rotation(rotation),
          scale(scale)
            {}

    TransformComponent::~TransformComponent() {
        delete Transform;
        Transform = nullptr;
    }

    bool TransformComponent::Initialize() {

        Transform = new TransformMatrix();
        UpdateTransform();
        auto world = GetTransformMatrix();
        RenderEngine::TransformBuffer tb(world,
                                         Camera::GetMainViewMatrix(),
                                         Camera::GetMainProjectionMatrix());
        ConstantBuffer = RefPtr<RenderEngine::BufferBase>(
            RenderEngine::ResourceManager::CreateBuffer(
                RenderEngine::BufferType::Constant,
                tb.GetSize(), tb.GetData()));

        return true;
    }

    void TransformComponent::Update(float DeltaTime) {
        UpdateConstantBuffer();

        RenderEngine::RenderCommandQueue::GetInstance()->SubmitBindConstantBuffer(ConstantBuffer, 0);
    }

    // Transformation matrix calculation
    Matrix4 TransformComponent::GetTransformMatrix() {
        return Transform->CalculateTransformMatrix();
    }

    // Transformations
    void TransformComponent::Translate(const Vector3D& translation) {
        position += translation;
        Transform->Translation = Matrix4::Translate(position);
        UpdateTransform();
    }


    void TransformComponent::Translate(float x, float y, float z) {
        Translate(Vector3D(x,y,z));
    }

    void TransformComponent::Rotate(const Quaternion& rotation) {
        this->rotation *= rotation;
        UpdateTransform();
    }

    void TransformComponent::Scale(float amount) {
        Scale(Vector3D(amount, amount, amount));
    }

    void TransformComponent::Scale(const Vector3D& scale) {
        this->scale = scale * this->scale;
        UpdateTransform();
    }

    // Setters
    void TransformComponent::SetPosition(const Vector3D& position) {
        this->position = position;
    }

    void TransformComponent::SetRotation(const Quaternion& rotation) {
        this->rotation = rotation;
    }

    void TransformComponent::SetScale(const Vector3D& scale) {
        this->scale = scale;
    }

    // Direction vectors
    Vector3D TransformComponent::Forward() const {
        return rotation * Vector3D(0.0f, 0.0f, 1.0f);
    }

    Vector3D TransformComponent::Right() const {
        return rotation * Vector3D(1.0f, 0.0f, 0.0f);
    }

    Vector3D TransformComponent::Up() const {
        return rotation * Vector3D(0.0f, 1.0f, 0.0f);
    }

    // Rotation utilities
    void TransformComponent::RotateAround(const Vector3D& axis, float angle) {
        Rotate(Quaternion(axis, angle));
    }

    void TransformComponent::RotateAroundLocal(const Vector3D& axis, float angle) {
        Rotate(Quaternion(rotation * axis, angle));
    }

    void TransformComponent::LookAt(const Vector3D& target) {
        Vector3D direction = (target - position).Normalize();
        Vector3D Up = VECTOR_Up;
        rotation = Quaternion::LookRotation(direction, Up);
    }

    // Getters
    Vector3D TransformComponent::GetWorldPosition() const {
        return position;
    }

    Quaternion TransformComponent::GetWorldRotation() const {
        return rotation;
    }

    Vector3D TransformComponent::GetWorldScale() const {
        return scale;
    }

    void TransformComponent::UpdateTransform() {
        Transform->Translation = Matrix4::Translate(position);
        Transform->Rotation = Matrix4::Rotate(rotation);
        Transform->Scaling = Matrix4::Scale(scale);

        Transform->CalculateTransformMatrix();
    }


    void TransformComponent::UpdateConstantBuffer() {
        if (!ConstantBuffer) {
            SLEAK_ERROR("Constant buffer is not found to update for object {}", owner->GetName());
            return;
        }

        UpdateTransform();

        auto world = GetTransformMatrix();
        RenderEngine::TransformBuffer tb(world,
                                         Camera::GetMainViewMatrix(),
                                         Camera::GetMainProjectionMatrix());

        RenderEngine::RenderCommandQueue::GetInstance()->SubmitUpdateConstantBuffer(ConstantBuffer, tb.GetData(), tb.GetSize());
    }


    Math::Matrix4 TransformComponent::GetMVP() {
        auto mat = GetTransformMatrix() *
                   Camera::GetMainViewMatrix() *
                   Camera::GetMainProjectionMatrix();

        return mat;
    }

}