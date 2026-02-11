#ifndef _CAMERACONTROLLERCOMPONENT_HPP_
#define _CAMERACONTROLLERCOMPONENT_HPP_

#include <ECS/Component.hpp>
#include <Math/Vector.hpp>
#include <Camera/Camera.hpp>
#include <Memory/RefPtr.h>

namespace Sleak {
    class CameraController : public Component {
    public:
        CameraController(GameObject* object) : Component(object) {}
        ~CameraController() override = default;

        virtual bool Initialize() {
            bIsInitialized = true;

            camera = GetCamera();

            return true;
        }

        virtual void Update(float deltaTime) = 0;

        virtual void ToggleCursor(bool enable) = 0;

        virtual void SetEnabled(bool enabled) {isEnabled = enabled;}
        virtual bool IsEnabled() const {return isEnabled;}
        virtual void SetSensitivity(float sensitivity) {this->sensitivity = sensitivity;}
        virtual float GetSensitivity() const {return sensitivity;}
       
        Camera* GetCamera() {
            return dynamic_cast<Camera*>(owner);
        }

    protected:
        virtual void UpdateCamera(float deltaTime) = 0;
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