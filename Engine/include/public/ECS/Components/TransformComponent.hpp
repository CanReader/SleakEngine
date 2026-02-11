#ifndef _TRANSFORM_COMPONENT_H_
#define _TRANSFORM_COMPONENT_H_

#include <ECS/Component.hpp>
#include "Math/Vector.hpp"
#include "Math/Quaternion.hpp"
#include "Math/Matrix.hpp"
#include <Memory/RefPtr.h>

namespace Sleak {

    using namespace Sleak::Math;

    namespace RenderEngine { class BufferBase; };

    /**
     * @class TransformComponent
     * @brief Represents the position, rotation, and scale of an entity in 3D space.
     *
     * This component is essential for positioning and orienting entities
     * within the game world. It provides functionalities for setting and
     * retrieving the transform properties, as well as calculating transformation
     * matrices for rendering and physics calculations.
     */
    class TransformComponent : public Component {
    public:
            // Constructors
         TransformComponent(GameObject* object, const Vector3D& position);
            TransformComponent(GameObject* object, const Vector3D& position,
                               const Quaternion& rotation);
         TransformComponent(GameObject* object, const Vector3D& position,
                            const Quaternion& rotation, const Vector3D& scale);
        ~TransformComponent() override;

        bool Initialize() override;
        void Update(float DeltaTime) override;

        // Transformation matrix calculation
        Matrix4 GetTransformMatrix();

        // Transformations
        void Translate(const Vector3D& translation);
        void Translate(float x, float y, float z);
        void Rotate(const Quaternion& rotation);
        void Scale(float amount);
        void Scale(const Vector3D& scale);

        // Setters
        void SetPosition(const Vector3D& position);
        void SetRotation(const Quaternion& rotation);
        void SetScale(const Vector3D& scale);

        // Direction vectors
        Vector3D Forward() const;
        Vector3D Right() const;
        Vector3D Up() const;

        // Rotation utilities
        void RotateAround(const Vector3D& axis, float angle);
        void RotateAroundLocal(const Vector3D& axis, float angle);
        void LookAt(const Vector3D& target);

        // Getters
        Vector3D GetWorldPosition() const;
        Quaternion GetWorldRotation() const;
        Vector3D GetWorldScale() const;

        struct TransformMatrix {
            
            TransformMatrix() : Translation(Matrix4::Identity()), Rotation(Matrix4::Identity()), Scaling(Matrix4::Identity()) {}

            Math::Matrix4 CalculateTransformMatrix() const {
                return (Scaling * Rotation) * Translation ;
            }

            Math::Matrix4 Translation;
            Math::Matrix4 Rotation;
            Math::Matrix4 Scaling;

        };

    protected:
        Vector3D position = Vector3D(0.0f, 0.0f, 0.0f);
        Quaternion rotation = Quaternion(); 
        Vector3D scale = Vector3D(1.0f, 1.0f, 1.0f);
        TransformMatrix* Transform;

    private:
        RefPtr<RenderEngine::BufferBase> ConstantBuffer;

        void UpdateConstantBuffer();
        void UpdateTransform();
        Math::Matrix4 GetMVP();
    };

} // namespace Sleak

#endif // _TRANSFORM_COMPONENT_H_

