#ifndef _FIRSTPERSONCONTROLLER_HPP_
#define _FIRSTPERSONCONTROLLER_HPP_

#include <ECS/Components/CameraController.hpp>
#include <Core/OSDef.hpp>
#include <Events/KeyboardEvent.hpp>
#include <string>

namespace Sleak {

    class RigidbodyComponent;

    /// Grounded first-person character controller with mouse look, jumping,
    /// sprinting, and an optional flight mode.
    ///
    /// Attach it to a Camera to get a player you can walk around with.
    /// Movement follows Unreal's CharacterMovementComponent model:
    /// acceleration toward a target speed, braking deceleration and ground
    /// friction when input stops, and deliberately low air control so a
    /// jump commits to its arc. `W`, `A`, `S`, `D` translate, the mouse
    /// looks, and pitch is clamped to plus or minus 89 degrees.
    ///
    /// It needs a sibling RigidbodyComponent, which Initialize() looks up:
    /// the rigidbody owns vertical motion and gravity, the controller owns
    /// horizontal velocity. Add a ColliderComponent too, or the character
    /// falls through the world.
    ///
    /// SetEnabled(false) releases the mouse and stops all input handling,
    /// which is what you want when a menu or console opens. Re-enabling
    /// resets velocity and re-syncs yaw and pitch to the camera's current
    /// facing, so the view does not snap. SetFlying(true) switches to
    /// noclip-style free flight, driven by SetVerticalFlyInput() for
    /// up and down.
    ///
    /// @code{.cpp}
    /// auto* cam = new Sleak::Camera("PlayerCamera",
    ///                               Sleak::Math::Vector3D(8, 70, 8),
    ///                               60.0f, 0.1f, 1500.0f);
    /// cam->AddComponent<Sleak::FirstPersonController>();
    /// cam->AddComponent<Sleak::ColliderComponent>(
    ///     Sleak::Physics::BoundingSphere(
    ///         Sleak::Math::Vector3D(0, 0, 0), 0.3f));
    /// cam->AddComponent<Sleak::RigidbodyComponent>(
    ///     Sleak::BodyType::Dynamic);
    ///
    /// if (auto* rb = cam->GetComponent<Sleak::RigidbodyComponent>()) {
    ///     rb->SetUseGravity(true);
    ///     rb->SetGravity(Sleak::Math::Vector3D(0.0f, -32.0f, 0.0f));
    /// }
    ///
    /// if (auto* fpc = cam->GetComponent<Sleak::FirstPersonController>()) {
    ///     fpc->SetMaxWalkSpeed(4.5f);
    ///     fpc->SetSprintSpeedMultiplier(1.8f);
    ///     fpc->SetJumpZVelocity(6.0f);
    ///     fpc->SetSensitivity(0.1f);
    /// }
    ///
    /// AddObject(cam);
    /// cam->Initialize();
    /// SetActiveCamera(cam);
    /// @endcode
    ///
    /// @see CameraController, FreeLookCameraController, Camera,
    ///      RigidbodyComponent, ColliderComponent
    /// @ingroup scene
    class ENGINE_API FirstPersonController : public CameraController {
    public:
        FirstPersonController(GameObject* object);
        ~FirstPersonController() override;

        /// Grabs the sibling RigidbodyComponent and derives initial yaw/pitch from the camera's facing.
        bool Initialize() override;
        void Update(float deltaTime) override;
        /// Toggles relative mouse mode and, when re-enabling, resets velocity and re-syncs yaw/pitch.
        void SetEnabled(bool enabled) override;

        void ToggleCursor(bool enable) override;

        void SetMaxWalkSpeed(float speed) { m_maxWalkSpeed = speed; }
        float GetMaxWalkSpeed() const { return m_maxWalkSpeed; }
        void SetSprintSpeedMultiplier(float mult) { m_sprintMultiplier = mult; }
        float GetSprintSpeedMultiplier() const { return m_sprintMultiplier; }
        void SetJumpZVelocity(float vel) { m_jumpZVelocity = vel; }
        float GetJumpZVelocity() const { return m_jumpZVelocity; }
        void SetMaxAcceleration(float accel) { m_maxAcceleration = accel; }
        void SetBrakingDeceleration(float decel) { m_brakingDeceleration = decel; }
        void SetGroundFriction(float friction) { m_groundFriction = friction; }
        void SetAirControl(float airControl) { m_airControl = airControl; }

        // Fly
        /// Switches between grounded movement and free-flight, resetting vertical velocity.
        void SetFlying(bool flying);
        bool IsFlying() const { return m_flying; }
        void SetMaxFlySpeed(float speed) { m_maxFlySpeed = speed; }
        float GetMaxFlySpeed() const { return m_maxFlySpeed; }
        void SetFlySprintMultiplier(float mult) { m_flySprintMultiplier = mult; }
        void SetVerticalFlyInput(float v) { m_flyVerticalInput = v; }

        float GetPitch() const { return m_pitch; }
        float GetYaw() const { return m_yaw; }
        void SetPitch(float pitch) { m_pitch = pitch; }
        void SetYaw(float yaw) { m_yaw = yaw; }

        /// Updates translation/sprint input state from a key-down event.
        void OnKeyPressed(const Events::Input::KeyPressedEvent& e);
        /// Clears translation/sprint input state from a key-up event.
        void OnKeyReleased(const Events::Input::KeyReleasedEvent& e);

    private:
        /// Reads mouse delta into yaw/pitch, clamped to m_pitchRange.
        void UpdateInput(float deltaTime) override;
        /// Applies UE-style acceleration/braking to velocity, resolves jump/fly, and moves the camera.
        void UpdateCamera(float deltaTime) override;

        // UE-style movement parameters — tuned slower for a calmer pace
        float m_maxWalkSpeed = 2.5f;             // Casual walking pace
        float m_sprintMultiplier = 2.0f;         // Sprint speed = walk * multiplier
        bool m_sprinting = false;
        float m_maxAcceleration = 10.0f;         // Gentle ramp-up (~0.25s to full speed)
        float m_brakingDeceleration = 12.0f;     // Smooth stop
        float m_groundFriction = 8.0f;           // UE default
        float m_airControl = 0.05f;              // Minimal air control

        // Jump — gentle hop, ~0.45m height
        // v = sqrt(2 * 9.81 * 0.45) ≈ 2.97
        float m_jumpZVelocity = 3.0f;

        // Fly state
        bool  m_flying = false;
        float m_maxFlySpeed = 10.0f;
        float m_flySprintMultiplier = 2.5f;
        float m_flyVerticalInput = 0.0f;
        float m_flyVerticalVelocity = 0.0f;

        // Mouse look
        float m_pitch = 0.0f;
        float m_yaw = 0.0f;
        Math::Vector2D m_pitchRange = Math::Vector2D(-89, 89);

        // Horizontal velocity (managed by controller, separate from rigidbody Y axis)
        Math::Vector3D m_velocity;

        // Input state
        bool m_firstFrame = true;

        RigidbodyComponent* m_rigidbody = nullptr;

        std::string m_keyPressedHandlerId;
        std::string m_keyReleasedHandlerId;
    };

} // namespace Sleak

#endif // _FIRSTPERSONCONTROLLER_HPP_
