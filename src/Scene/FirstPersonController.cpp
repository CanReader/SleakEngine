#include <Core/Application.hpp>
#include <Core/Window.hpp>
#include <ECS/Components/FirstPersonController.hpp>
#include <Input/Keyboard.hpp>
#include <Input/Mouse.hpp>
#include <Math/Math.hpp>
#include <Math/Quaternion.hpp>
#include <Physics/RigidbodyComponent.hpp>
#include <cmath>

namespace Sleak {

FirstPersonController::FirstPersonController(GameObject* object)
    : CameraController(object) {
    sensitivity = 0.002f;
    m_velocity = Math::Vector3D::Zero();
    translationInput = Math::Vector3D::Zero();
}

FirstPersonController::~FirstPersonController() = default;

bool FirstPersonController::Initialize() {
    if (!CameraController::Initialize())
        return false;

    m_rigidbody = owner->GetComponent<RigidbodyComponent>();

    if (camera) {
        Math::Vector3D forward = camera->GetDirection();
        m_yaw = atan2(forward.GetX(), forward.GetZ());
        m_pitch = -asin(forward.GetY());
        m_pitch = Math::Clamp(m_pitch,
            static_cast<float>(m_pitchRange.GetX() * D2R),
            static_cast<float>(m_pitchRange.GetY() * D2R));
    }

    return true;
}

void FirstPersonController::Update(float deltaTime) {
    if (!bIsInitialized || !isEnabled) return;

    UpdateInput(deltaTime);
    UpdateCamera(deltaTime);
}

void FirstPersonController::ToggleCursor(bool enabled) {
    Input::Mouse::SetCursorVisible(enabled);
}

void FirstPersonController::UpdateInput(float deltaTime) {
    using Input::KEY_CODE;
    using Input::Keyboard;

    auto axis = [](KEY_CODE positive, KEY_CODE negative) {
        return (Keyboard::IsKeyHold(positive) ? 1.0f : 0.0f) -
               (Keyboard::IsKeyHold(negative) ? 1.0f : 0.0f);
    };
    translationInput.SetZ(axis(KEY_CODE::KEY__W, KEY_CODE::KEY__S));
    translationInput.SetX(axis(KEY_CODE::KEY__A, KEY_CODE::KEY__D));
    if (Keyboard::IsKeyPressed(KEY_CODE::KEY__SPACE)) {
        translationInput.SetY(1.0f);
    } else if (Keyboard::IsKeyReleased(KEY_CODE::KEY__SPACE)) {
        translationInput.SetY(0.0f);
    }
    m_sprinting = Keyboard::IsKeyHold(KEY_CODE::KEY__LSHIFT) ||
                  Keyboard::IsKeyHold(KEY_CODE::KEY__RSHIFT);

    Math::Vector2D delta = Input::Mouse::GetDelta();
    float x = delta.GetX();
    float y = delta.GetY();

    if (m_firstFrame) {
        m_firstFrame = false;
        return;
    }

    // Direct mouse input — no smoothing, 1:1 like UE
    m_yaw += x * sensitivity;
    m_pitch += y * sensitivity;

    m_pitch = Math::Clamp(m_pitch,
        static_cast<float>(m_pitchRange.GetX() * D2R),
        static_cast<float>(m_pitchRange.GetY() * D2R));
}

void FirstPersonController::UpdateCamera(float deltaTime) {
    if (!camera) return;

    Math::Quaternion yawRotation = Math::Quaternion(Math::Vector3D::Up(), m_yaw);
    Math::Quaternion pitchRotation = Math::Quaternion(Math::Vector3D::Right(), m_pitch);
    Math::Quaternion combinedRotation = yawRotation * pitchRotation;

    Math::Vector3D forward = combinedRotation * Math::Vector3D::Forward();

    // XZ-projected movement axes (ground-locked)
    Math::Vector3D flatForward(forward.GetX(), 0.0f, forward.GetZ());
    if (flatForward.Magnitude() > 0.001f) {
        flatForward = flatForward.Normalized();
    } else {
        flatForward = Math::Vector3D::Forward();
    }
    Math::Vector3D right = flatForward.Cross(Math::Vector3D::Up()).Normalized();

    Math::Vector3D inputDir = right * translationInput.GetX() + flatForward * translationInput.GetZ();
    bool hasInput = inputDir.Magnitude() > 0.001f;
    if (hasInput) {
        inputDir = inputDir.Normalized();
    }

    bool isGrounded = m_rigidbody && m_rigidbody->IsGrounded();

    // Horizontal speed + accel (per mode)
    float currentMaxSpeed;
    float effectiveAccel;
    float effectiveBraking;
    if (m_flying) {
        currentMaxSpeed = m_sprinting ? m_maxFlySpeed * m_flySprintMultiplier
                                      : m_maxFlySpeed;
        float walkRef = (m_maxWalkSpeed > 0.001f) ? m_maxWalkSpeed : 1.0f;
        float speedRatio = currentMaxSpeed / walkRef;
        effectiveAccel   = m_maxAcceleration   * speedRatio;
        effectiveBraking = m_brakingDeceleration * speedRatio;
    } else {
        currentMaxSpeed = m_sprinting ? m_maxWalkSpeed * m_sprintMultiplier
                                      : m_maxWalkSpeed;
        effectiveAccel = m_maxAcceleration;
        if (!isGrounded) effectiveAccel *= m_airControl;
        effectiveBraking = m_brakingDeceleration;
        if (isGrounded) effectiveBraking *= m_groundFriction;
    }

    float speed = m_velocity.Magnitude();

    if (hasInput) {
        m_velocity = m_velocity + inputDir * effectiveAccel * deltaTime;

        float newSpeed = m_velocity.Magnitude();
        if (newSpeed > currentMaxSpeed) {
            m_velocity = m_velocity * (currentMaxSpeed / newSpeed);
        }
    } else {
        if (speed > 0.01f) {
            float drop = effectiveBraking * deltaTime;
            float newSpeed = speed - drop;
            if (newSpeed < 0.0f) newSpeed = 0.0f;
            m_velocity = m_velocity * (newSpeed / speed);
        } else {
            m_velocity = Math::Vector3D::Zero();
        }
    }

    // Wall sliding: cancel velocity pushing into walls
    if (m_rigidbody && m_rigidbody->HadWallCollision()) {
        Math::Vector3D wallNormal = m_rigidbody->GetWallNormal();
        float dot = m_velocity.Dot(wallNormal);
        if (dot < 0.0f) {
            m_velocity = m_velocity - wallNormal * dot;
        }
    }

    // Vertical: jump (ground) or fly
    if (m_flying) {
        if (m_flyVerticalInput > 0.001f || m_flyVerticalInput < -0.001f) {
            m_flyVerticalVelocity += m_flyVerticalInput * effectiveAccel * deltaTime;
            if (m_flyVerticalVelocity >  currentMaxSpeed) m_flyVerticalVelocity =  currentMaxSpeed;
            if (m_flyVerticalVelocity < -currentMaxSpeed) m_flyVerticalVelocity = -currentMaxSpeed;
        } else {
            float vSpeed = std::fabs(m_flyVerticalVelocity);
            if (vSpeed > 0.01f) {
                float drop = effectiveBraking * deltaTime;
                float newSpeed = vSpeed - drop;
                if (newSpeed < 0.0f) newSpeed = 0.0f;
                m_flyVerticalVelocity *= (newSpeed / vSpeed);
            } else {
                m_flyVerticalVelocity = 0.0f;
            }
        }
        Math::Vector3D moveVel(m_velocity.GetX(),
                               m_flyVerticalVelocity,
                               m_velocity.GetZ());
        camera->AddPosition(moveVel * deltaTime);
    } else {
        camera->AddPosition(m_velocity * deltaTime);

        if (m_rigidbody && isGrounded) {
            if (translationInput.GetY() > 0.0f) {
                Math::Vector3D vel = m_rigidbody->GetVelocity();
                vel = Math::Vector3D(vel.GetX(), m_jumpZVelocity, vel.GetZ());
                m_rigidbody->SetVelocity(vel);
                m_rigidbody->SetGrounded(false);
                translationInput.SetY(0.0f);
            }
        }
    }

    // Update look target
    Math::Vector3D lookTarget = camera->GetPosition() + forward;
    camera->SetLookTarget(lookTarget);
}

void FirstPersonController::OnKeyPressed(
    const Events::Input::KeyPressedEvent&) {}

void FirstPersonController::OnKeyReleased(
    const Events::Input::KeyReleasedEvent&) {}

void FirstPersonController::SetFlying(bool flying) {
    if (m_flying == flying) return;
    m_flying = flying;
    m_flyVerticalVelocity = 0.0f;
    m_flyVerticalInput = 0.0f;
    translationInput.SetY(0.0f);
}

void FirstPersonController::SetEnabled(bool enabled) {
    CameraController::SetEnabled(enabled);

    if (!camera) return;

    auto* app = Application::GetInstance();
    if (app) app->GetWindow().SetRelativeMouseMode(enabled);
    ToggleCursor(!enabled);

    if (enabled) {
        m_velocity = Math::Vector3D::Zero();
        translationInput = Math::Vector3D::Zero();
        m_firstFrame = true;

        Math::Vector3D forward = camera->GetDirection();
        m_yaw = atan2(forward.GetX(), forward.GetZ());
        m_pitch = -asin(forward.GetY());
        m_pitch = Math::Clamp(m_pitch,
            static_cast<float>(m_pitchRange.GetX() * D2R),
            static_cast<float>(m_pitchRange.GetY() * D2R));
    }
}

} // namespace Sleak
