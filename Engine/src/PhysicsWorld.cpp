#include <Physics/PhysicsWorld.hpp>
#include <Physics/ColliderComponent.hpp>
#include <Physics/RigidbodyComponent.hpp>
#include <Core/GameObject.hpp>
#include <Camera/Camera.hpp>
#include <ECS/Components/TransformComponent.hpp>
#include <Logger.hpp>
#include <algorithm>

namespace Sleak {
namespace Physics {

void PhysicsWorld::RegisterCollider(ColliderComponent* collider) {
    if (!collider) return;

    // Check not already registered
    for (auto* c : m_colliders) {
        if (c == collider) return;
    }

    AABB worldAABB = collider->GetWorldAABB();
    int proxyId = m_tree.Insert(worldAABB, collider);
    collider->SetProxyId(proxyId);
    m_colliders.push_back(collider);
}

void PhysicsWorld::UnregisterCollider(ColliderComponent* collider) {
    if (!collider) return;

    int proxyId = collider->GetProxyId();
    if (proxyId >= 0) {
        m_tree.Remove(proxyId);
        collider->SetProxyId(-1);
    }

    m_colliders.erase(
        std::remove(m_colliders.begin(), m_colliders.end(), collider),
        m_colliders.end());
}

void PhysicsWorld::Step(float dt) {
    // Phase 1: Save grounded state, then clear collision flags
    // We need wasGrounded BEFORE clearing, so gravity doesn't apply while standing
    for (auto* collider : m_colliders) {
        if (auto* owner = collider->GetOwner()) {
            auto* rb = owner->GetComponent<RigidbodyComponent>();
            if (!rb) continue;

            bool wasGrounded = rb->IsGrounded();
            rb->ClearCollisionState();

            if (rb->GetBodyType() == BodyType::Dynamic) {
                // Reset grounded — collision detection will re-set it if still touching ground
                rb->SetGrounded(false);

                // Phase 2: Gravity integration
                if (rb->GetUseGravity()) {
                    Math::Vector3D vel = rb->GetVelocity();

                    if (!wasGrounded) {
                        // Airborne: apply full gravity
                        vel = vel + rb->GetGravity() * dt;
                    } else {
                        // Grounded: apply small downward force to maintain ground contact
                        // This ensures collision detection keeps finding the floor
                        // without causing visible jitter
                        if (vel.GetY() <= 0.0f) {
                            vel = Math::Vector3D(vel.GetX(), -0.5f, vel.GetZ());
                        }
                        // If vel.Y > 0 (jumping), don't override — let the jump happen
                    }

                    // Clamp to terminal velocity
                    float termVel = rb->GetTerminalVelocity();
                    if (vel.GetY() < -termVel) {
                        vel = Math::Vector3D(vel.GetX(), -termVel, vel.GetZ());
                    }

                    rb->SetVelocity(vel);
                }

                // Phase 3: Integrate velocity into position
                Math::Vector3D vel = rb->GetVelocity();
                Math::Vector3D delta = vel * dt;

                auto* transform = owner->GetComponent<TransformComponent>();
                if (transform) {
                    transform->Translate(delta);
                } else if (auto* cam = dynamic_cast<Camera*>(owner)) {
                    cam->AddPosition(delta);
                }
            }
        }
    }

    // Phase 4: Broadphase update and collision detection/resolution
    UpdateBroadphase();
    FindPairsAndResolve();
}

void PhysicsWorld::UpdateBroadphase() {
    for (auto* collider : m_colliders) {
        int proxyId = collider->GetProxyId();
        if (proxyId < 0) continue;

        AABB newAABB = collider->GetWorldAABB();
        m_tree.MoveProxy(proxyId, newAABB, Vector3D(0, 0, 0));
    }
}

void PhysicsWorld::FindPairsAndResolve() {
    for (size_t i = 0; i < m_colliders.size(); ++i) {
        ColliderComponent* colliderA = m_colliders[i];
        if (colliderA->GetProxyId() < 0) continue;

        AABB worldA = colliderA->GetWorldAABB();

        m_tree.Query(worldA, [&](int proxyId) -> bool {
            auto* colliderB = static_cast<ColliderComponent*>(m_tree.GetUserData(proxyId));
            if (colliderB == colliderA) return true;

            // Skip duplicate pairs: only process when A < B (pointer order)
            if (colliderA > colliderB) return true;

            // Layer filtering
            if ((colliderA->GetLayer() & colliderB->GetMask()) == 0) return true;
            if ((colliderB->GetLayer() & colliderA->GetMask()) == 0) return true;

            // Get transform data
            auto* ownerA = colliderA->GetOwner();
            auto* ownerB = colliderB->GetOwner();
            if (!ownerA || !ownerB) return true;

            Vector3D posA, posB;
            Vector3D scaleA(1, 1, 1), scaleB(1, 1, 1);

            auto* transformA = ownerA->GetComponent<TransformComponent>();
            if (transformA) {
                posA = transformA->GetWorldPosition() + colliderA->GetOffset();
                scaleA = transformA->GetWorldScale();
            } else if (auto* camA = dynamic_cast<Camera*>(ownerA)) {
                posA = camA->GetPosition() + colliderA->GetOffset();
            }

            auto* transformB = ownerB->GetComponent<TransformComponent>();
            if (transformB) {
                posB = transformB->GetWorldPosition() + colliderB->GetOffset();
                scaleB = transformB->GetWorldScale();
            } else if (auto* camB = dynamic_cast<Camera*>(ownerB)) {
                posB = camB->GetPosition() + colliderB->GetOffset();
            }

            CollisionManifold manifold = TestCollision(
                colliderA->GetShape(), posA, scaleA,
                colliderB->GetShape(), posB, scaleB);

            if (!manifold.hasCollision) return true;

            // Skip if both are triggers
            if (colliderA->IsTrigger() && colliderB->IsTrigger()) return true;

            // Resolve: push rigidbodies apart
            auto* rbA = ownerA->GetComponent<RigidbodyComponent>();
            auto* rbB = ownerB->GetComponent<RigidbodyComponent>();

            if (rbA && rbA->GetBodyType() != BodyType::Static) {
                rbA->ResolveCollision(manifold.contact.normal * -1.0f,
                                      manifold.contact.penetration);
            }
            if (rbB && rbB->GetBodyType() != BodyType::Static) {
                rbB->ResolveCollision(manifold.contact.normal,
                                      manifold.contact.penetration);
            }

            return true;
        });
    }
}

// --- Query API ---

std::vector<CollisionPair> PhysicsWorld::OverlapSphere(const Vector3D& center, float radius, uint32_t layerMask) const {
    std::vector<CollisionPair> results;
    BoundingSphere sphere(center, radius);
    AABB queryAABB = sphere.ToAABB();

    m_tree.Query(queryAABB, [&](int proxyId) -> bool {
        auto* collider = static_cast<ColliderComponent*>(m_tree.GetUserData(proxyId));
        if ((collider->GetLayer() & layerMask) == 0) return true;

        CollisionPair pair;
        pair.b = collider;
        results.push_back(pair);
        return true;
    });

    return results;
}

std::vector<CollisionPair> PhysicsWorld::OverlapAABB(const AABB& aabb, uint32_t layerMask) const {
    std::vector<CollisionPair> results;

    m_tree.Query(aabb, [&](int proxyId) -> bool {
        auto* collider = static_cast<ColliderComponent*>(m_tree.GetUserData(proxyId));
        if ((collider->GetLayer() & layerMask) == 0) return true;

        CollisionPair pair;
        pair.b = collider;
        results.push_back(pair);
        return true;
    });

    return results;
}

SweepResult PhysicsWorld::SphereSweep(const Vector3D& start, const Vector3D& direction,
                                       float radius, float maxDist, uint32_t layerMask) const {
    SweepResult result;

    // Expand ray into a fat AABB for broadphase query
    Vector3D end = start + direction * maxDist;
    AABB sweepAABB(
        Vector3D(std::min(start.GetX(), end.GetX()) - radius,
                 std::min(start.GetY(), end.GetY()) - radius,
                 std::min(start.GetZ(), end.GetZ()) - radius),
        Vector3D(std::max(start.GetX(), end.GetX()) + radius,
                 std::max(start.GetY(), end.GetY()) + radius,
                 std::max(start.GetZ(), end.GetZ()) + radius)
    );

    float closestDist = maxDist;

    m_tree.Query(sweepAABB, [&](int proxyId) -> bool {
        auto* collider = static_cast<ColliderComponent*>(m_tree.GetUserData(proxyId));
        if ((collider->GetLayer() & layerMask) == 0) return true;

        // Step along the sweep direction testing sphere collisions
        AABB targetAABB = collider->GetWorldAABB();
        Vector3D targetCenter = targetAABB.GetCenter();
        Vector3D targetExtents = targetAABB.GetExtents();

        // Expand target AABB by sweep radius for simplified test
        AABB expandedTarget(
            Vector3D(targetAABB.min.GetX() - radius,
                     targetAABB.min.GetY() - radius,
                     targetAABB.min.GetZ() - radius),
            Vector3D(targetAABB.max.GetX() + radius,
                     targetAABB.max.GetY() + radius,
                     targetAABB.max.GetZ() + radius)
        );

        // Ray vs expanded AABB
        Vector3D invDir(
            std::abs(direction.GetX()) > 1e-8f ? 1.0f / direction.GetX() : 1e8f,
            std::abs(direction.GetY()) > 1e-8f ? 1.0f / direction.GetY() : 1e8f,
            std::abs(direction.GetZ()) > 1e-8f ? 1.0f / direction.GetZ() : 1e8f
        );

        float t1x = (expandedTarget.min.GetX() - start.GetX()) * invDir.GetX();
        float t2x = (expandedTarget.max.GetX() - start.GetX()) * invDir.GetX();
        float t1y = (expandedTarget.min.GetY() - start.GetY()) * invDir.GetY();
        float t2y = (expandedTarget.max.GetY() - start.GetY()) * invDir.GetY();
        float t1z = (expandedTarget.min.GetZ() - start.GetZ()) * invDir.GetZ();
        float t2z = (expandedTarget.max.GetZ() - start.GetZ()) * invDir.GetZ();

        float tmin = std::max({std::min(t1x, t2x), std::min(t1y, t2y), std::min(t1z, t2z)});
        float tmax = std::min({std::max(t1x, t2x), std::max(t1y, t2y), std::max(t1z, t2z)});

        if (tmax < 0 || tmin > tmax || tmin > closestDist) return true;

        float hitDist = std::max(tmin, 0.0f);
        if (hitDist < closestDist) {
            closestDist = hitDist;
            result.hit = true;
            result.collider = collider;
            result.distance = hitDist;
            result.point = start + direction * hitDist;

            // Compute approximate normal from hit point
            Vector3D hitPt = result.point;
            Vector3D diff = hitPt - targetCenter;

            // Find which face we hit
            float ax = std::abs(diff.GetX()) / std::max(targetExtents.GetX(), 0.001f);
            float ay = std::abs(diff.GetY()) / std::max(targetExtents.GetY(), 0.001f);
            float az = std::abs(diff.GetZ()) / std::max(targetExtents.GetZ(), 0.001f);

            if (ax > ay && ax > az) {
                result.normal = Vector3D(diff.GetX() > 0 ? 1.0f : -1.0f, 0, 0);
            } else if (ay > az) {
                result.normal = Vector3D(0, diff.GetY() > 0 ? 1.0f : -1.0f, 0);
            } else {
                result.normal = Vector3D(0, 0, diff.GetZ() > 0 ? 1.0f : -1.0f);
            }
        }

        return true;
    });

    return result;
}

RayHit PhysicsWorld::Raycast(const Vector3D& origin, const Vector3D& direction,
                              float maxDist, uint32_t layerMask) const {
    // Raycast is sphere sweep with radius 0, but use direct ray-AABB for better precision
    SweepResult sweep = SphereSweep(origin, direction, 0.01f, maxDist, layerMask);
    RayHit result;
    result.hit = sweep.hit;
    result.collider = sweep.collider;
    result.point = sweep.point;
    result.normal = sweep.normal;
    result.distance = sweep.distance;
    return result;
}

} // namespace Physics
} // namespace Sleak
