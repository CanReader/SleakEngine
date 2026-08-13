#include <ECS/Components/FirstPersonController.hpp>
#include <Physics/RigidbodyComponent.hpp>
#include <Core/Application.hpp>
#include <Core/Window.hpp>
#include <SDL3/SDL.h>
#include <Events/Event.h>
#include <Math/Math.hpp>
#include <Math/Quaternion.hpp>
#include <cmath>

namespace Sleak {

FirstPersonController::FirstPersonController(GameObject* object)
    : CameraController(object) {
    sensitivity = 0.002f;
    m_velocity = Math::Vector3D::Zero();
    translationInput = Math::Vector3D::Zero();

    m_keyPressedHandlerId  = EventDispatcher::RegisterEventHandler(this, &FirstPersonController::OnKeyPressed);
    m_keyReleasedHandlerId = EventDispatcher::RegisterEventHandler(this, &FirstPersonController::OnKeyReleased);
}

FirstPersonController::~FirstPersonController() {
    EventDispatcher::UnregisterEvent(EventType::KeyPressed,  m_keyPressedHandlerId);
    EventDispatcher::UnregisterEvent(EventType::KeyReleased, m_keyReleasedHandlerId);
}

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
    bool cursor_set = enabled ? SDL_ShowCursor() : SDL_HideCursor();
    (void)cursor_set;
}

void FirstPersonController::UpdateInput(float deltaTime) {
    float x, y;
    SDL_GetRelativeMouseState(&x, &y);

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

void FirstPersonController::OnKeyPressed(const Events::Input::KeyPressedEvent& e) {
    if (!isEnabled) return;

    switch (e.GetKeyCode()) {
        case Input::KEY_CODE::KEY__W: translationInput.SetZ(1.0f); break;
        case Input::KEY_CODE::KEY__S: translationInput.SetZ(-1.0f); break;
        case Input::KEY_CODE::KEY__A: translationInput.SetX(+1.0f); break;
        case Input::KEY_CODE::KEY__D: translationInput.SetX(-1.0f); break;
        case Input::KEY_CODE::KEY__SPACE:
            if (!e.IsRepeat()) {
                translationInput.SetY(1.0f);
            }
            break;
        case Input::KEY_CODE::KEY__LSHIFT:
        case Input::KEY_CODE::KEY__RSHIFT:
            m_sprinting = true;
            break;
        default: break;
    }
}

void FirstPersonController::OnKeyReleased(const Events::Input::KeyReleasedEvent& e) {
    if (!isEnabled) return;

    switch (e.GetKeyCode()) {
        case Input::KEY_CODE::KEY__W:
        case Input::KEY_CODE::KEY__S: translationInput.SetZ(0.0f); break;
        case Input::KEY_CODE::KEY__A:
        case Input::KEY_CODE::KEY__D: translationInput.SetX(0.0f); break;
        case Input::KEY_CODE::KEY__SPACE:
            translationInput.SetY(0.0f);
            break;
        case Input::KEY_CODE::KEY__LSHIFT:
        case Input::KEY_CODE::KEY__RSHIFT:
            m_sprinting = false;
            break;
        default: break;
    }
}

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
