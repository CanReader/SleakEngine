#ifndef _FREELOOKCOMPONENT_HPP_
#define _FREELOOKCOMPONENT_HPP_

#include <ECS/Components/CameraController.hpp>
#include <Events/KeyboardEvent.h>

namespace Sleak {
    class FreeLookCameraController : public CameraController {
    public:
        FreeLookCameraController(GameObject* object);

        bool Initialize() override;
        void Update(float deltaTime) override;
        void SetEnabled(bool enabled) override;

        void ToggleCursor(bool enable) override;

        virtual void SetSpeed(float speed) {this->speed = speed;}
        virtual float GetSpeed() const {return speed;}
        virtual void SetAcceleration(float acceleration) {this->acceleration = acceleration;}
        virtual float GetAcceleration() const {return acceleration;}
        virtual void SetDamping(float damping) {this->damping = damping;}
        virtual float GetDamping() const {return damping;}
        virtual void SetMaxSpeed(float maxSpeed) {this->maxSpeed = maxSpeed;}
        virtual float GetMaxSpeed() const {return maxSpeed;}
        virtual Math::Vector3D GetVelocity() const { return velocity;}

        Math::Vector3D GetTranslationInput() const { return translationInput; }

        void SetInvertY(bool invert) { isInvertY = invert; }
        bool GetInvertY() const { return isInvertY; }
        
        void SetPitchRange(Math::Vector2D range) { PitchRange = range; }
        Math::Vector2D GetPitchRange() const { return PitchRange; }
        void SetYawRange(Math::Vector2D range) { YawRange = range; }
        Math::Vector2D GetYawRange() const { return YawRange; }
        void SetRollRange(Math::Vector2D range) { RollRange = range; }
        Math::Vector2D GetRollRange() const { return RollRange; }

        void SetPitch(float pitch) { this->pitch = pitch; }
        float GetPitch() const { return pitch; }
        void SetYaw(float yaw) { this->yaw = yaw; }
        float GetYaw() const { return yaw; }
        void SetRoll(float roll) { this->roll = roll; }
        float GetRoll() const { return roll; }


        virtual void OnKeyPressed(const Sleak::Events::Input::KeyPressedEvent& e);
        virtual void OnKeyReleased(const Sleak::Events::Input::KeyReleasedEvent& e);

    private:
        void UpdateInput(float deltaTime) override;
        void UpdateCamera(float deltaTime) override;

        void ApplyDamping(float DeltaTime);
        void ApplyAcceleration(float DeltaTime);
        void ClampVelocity();

        Math::Vector3D velocity;        
        float speed = 1.0f;
        float sensitivity = 0.1f;
        float acceleration;
        float damping;
        float maxSpeed;
        
        bool isInvertY = false;
        float pitch = 0.0f;
        float yaw = 0.0f;
        float roll = 0.0f;
        Math::Vector2D PitchRange;
        Math::Vector2D YawRange;
        Math::Vector2D RollRange;

        bool m_firstFrame = true;
    };
}

#endif