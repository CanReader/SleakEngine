#ifndef _CAMERACONTROLLERCOMPONENT_HPP_
#define _CAMERACONTROLLERCOMPONENT_HPP_

#include <ECS/Component.hpp>
#include <Math/Vector.hpp>
#include <Camera/Camera.hpp>
#include <Memory/RefPtr.hpp>
#include <Core/OSDef.hpp>

namespace Sleak {
    /// Base for components that drive a Camera GameObject from input.
    /// Concrete controllers (first-person, free-look) implement UpdateInput/UpdateCamera.
    class ENGINE_API CameraController : public Component {
    public:
        CameraController(GameObject* object) : Component(object) {}
        ~CameraController() override = default;

        virtual bool Initialize() {
            bIsInitialized = true;

            camera = GetCamera();

            return true;
        }

        virtual void Update(float deltaTime) = 0;

        /// Shows or hides the OS cursor and, typically, toggles relative mouse mode.
        virtual void ToggleCursor(bool enable) = 0;

        virtual void SetEnabled(bool enabled) {isEnabled = enabled;}
        virtual bool IsEnabled() const {return isEnabled;}
        virtual void SetSensitivity(float sensitivity) {this->sensitivity = sensitivity;}
        virtual float GetSensitivity() const {return sensitivity;}

        /// Casts the owning GameObject to Camera; valid only when attached to one.
        Camera* GetCamera() {
            return dynamic_cast<Camera*>(owner);
        }

    protected:
        /// Applies accumulated input to the camera's position/orientation.
        virtual void UpdateCamera(float deltaTime) = 0;
        /// Polls raw input devices and updates translation/rotation input state.
        virtual void UpdateInput(float deltaTime) = 0;

        float sensitivity = 0.0f;
        bool isEnabled = true;
        
        Math::Vector3D translationInput;
        Math::Vector3D rotationInput;
        Math::Vector2D MousePosition, LastMousePosition;
        
        Camera* camera = nullptr;
    };
}

#endif