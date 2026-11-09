#include <ECS/Components/TransformComponent.hpp>
#include <Graphics/Common/BufferBase.hpp>
#include <Graphics/Common/ConstantBuffer.hpp>
#include <Graphics/Common/ResourceManager.hpp>
#include <Camera/Camera.hpp>
#include <Graphics/Common/RenderCommandQueue.hpp>
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

    TransformComponent::~TransformComponent() = default;

    bool TransformComponent::Initialize() {
        if (ConstantBuffer.IsValid()) return true;

        RenderEngine::TransformBuffer tb(WorldMatrix(),
                                         Camera::GetMainViewMatrix(),
                                         Camera::GetMainProjectionMatrix());
        ConstantBuffer = RefPtr<RenderEngine::BufferBase>(
            RenderEngine::ResourceManager::CreateBuffer(
                RenderEngine::BufferType::Constant,
                tb.GetSize(), tb.GetData()));

        return true;
    }

    void TransformComponent::Update(float DeltaTime) {
        MeshComponent* mesh =
            owner ? owner->GetComponent<MeshComponent>() : nullptr;
        if (mesh && mesh->IsInitialized()) return;

        SubmitRenderState();
    }

    void TransformComponent::SubmitRenderState() {
        UpdateConstantBuffer();

        RenderEngine::RenderCommandQueue::GetInstance()->SubmitBindConstantBuffer(ConstantBuffer, 0);
    }

    Matrix4 TransformComponent::GetTransformMatrix() { return WorldMatrix(); }

    const Matrix4& TransformComponent::WorldMatrix() {
        if (m_worldDirty) {
            m_world = (Matrix4::Scale(scale) * Matrix4::Rotate(rotation)) *
                      Matrix4::Translate(position);
            m_worldDirty = false;
        }
        return m_world;
    }

    // Transformations
    void TransformComponent::Translate(const Vector3D& translation) {
        position += translation;
        m_worldDirty = true;
    }


    void TransformComponent::Translate(float x, float y, float z) {
        Translate(Vector3D(x,y,z));
    }

    void TransformComponent::Rotate(const Quaternion& rotation) {
        this->rotation *= rotation;
        m_worldDirty = true;
    }

    void TransformComponent::Scale(float amount) {
        Scale(Vector3D(amount, amount, amount));
    }

    void TransformComponent::Scale(const Vector3D& scale) {
        this->scale = scale * this->scale;
        m_worldDirty = true;
    }

    // Setters
    void TransformComponent::SetPosition(const Vector3D& position) {
        this->position = position;
        m_worldDirty = true;
    }

    void TransformComponent::SetRotation(const Quaternion& rotation) {
        this->rotation = rotation;
        m_worldDirty = true;
    }

    void TransformComponent::SetScale(const Vector3D& scale) {
        this->scale = scale;
        m_worldDirty = true;
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
        m_worldDirty = true;
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

    void TransformComponent::UpdateConstantBuffer() {
        if (!ConstantBuffer) {
            SLEAK_ERROR("Constant buffer is not found to update for object {}", owner->GetName());
            return;
        }

        RenderEngine::TransformBuffer tb(WorldMatrix(),
                                         Camera::GetMainViewMatrix(),
                                         Camera::GetMainProjectionMatrix());

        RenderEngine::RenderCommandQueue::GetInstance()->SubmitUpdateConstantBuffer(ConstantBuffer, tb.GetData(), tb.GetSize());
    }


    Math::Matrix4 TransformComponent::GetMVP() {
        auto mat = WorldMatrix() * Camera::GetMainViewMatrix() *
                   Camera::GetMainProjectionMatrix();

        return mat;
    }

}