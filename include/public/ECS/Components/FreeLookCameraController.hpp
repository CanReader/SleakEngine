#ifndef _FREELOOKCOMPONENT_HPP_
#define _FREELOOKCOMPONENT_HPP_

#include <ECS/Components/CameraController.hpp>
#include <Events/KeyboardEvent.hpp>
#include <Core/OSDef.hpp>
#include <string>

namespace Sleak {
    /// Editor/spectator-style camera controller: mouse-look plus accelerated
    /// free translation, independent of gravity or collision.
    class ENGINE_API FreeLookCameraController : public CameraController {
    public:
        FreeLookCameraController(GameObject* object);
        ~FreeLookCameraController() override;

        /// Derives initial yaw/pitch from the camera's current facing direction.
        bool Initialize() override;
        void Update(float deltaTime) override;
        /// Toggles relative mouse mode and, when re-enabling, resets velocity and re-syncs yaw/pitch.
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


        /// Updates translation input state from a key-down event; doubles speed while LCTRL is held.
        virtual void OnKeyPressed(const Sleak::Events::Input::KeyPressedEvent& e);
        /// Clears translation input state from a key-up event.
        virtual void OnKeyReleased(const Sleak::Events::Input::KeyReleasedEvent& e);

    private:
        /// Reads and smooths mouse delta into yaw/pitch, clamped to PitchRange.
        void UpdateInput(float deltaTime) override;
        /// Accelerates toward the input-driven target velocity, applies damping, and moves the camera.
        void UpdateCamera(float deltaTime) override;

        /// Exponentially decays velocity toward zero when there's no translation input.
        void ApplyDamping(float DeltaTime);
        /// Accelerates velocity toward the input-driven target velocity.
        void ApplyAcceleration(float DeltaTime);
        /// Clamps velocity magnitude to maxSpeed.
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

        std::string m_keyPressedHandlerId;
        std::string m_keyReleasedHandlerId;
    };
}

#endif