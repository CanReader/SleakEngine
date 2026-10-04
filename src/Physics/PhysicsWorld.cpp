#include <Core/GameObject.hpp>
#include <Core/Logger.hpp>
#include <ECS/Components/TransformComponent.hpp>
#include <Physics/ColliderComponent.hpp>
#include <Physics/PhysicsWorld.hpp>
#include <Physics/RigidbodyComponent.hpp>
#include <algorithm>
#include <cmath>

namespace Sleak {
namespace Physics {

namespace {

enum class ContactCallback { Enter, Stay, Exit };

uint64_t PairKey(int proxyA, int proxyB) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(proxyA)) << 32) |
           static_cast<uint32_t>(proxyB);
}

bool IsMovable(const RigidbodyComponent* rb) {
    return rb && rb->GetBodyType() != BodyType::Static;
}

/// Share of the separation each body takes; zero means it does not move.
void SeparationShares(RigidbodyComponent* rbA, RigidbodyComponent* rbB,
                      float& shareA, float& shareB) {
    bool movA = IsMovable(rbA);
    bool movB = IsMovable(rbB);
    shareA = shareB = 0.0f;

    if (movA && movB) {
        bool dynA = rbA->GetBodyType() == BodyType::Dynamic;
        bool dynB = rbB->GetBodyType() == BodyType::Dynamic;
        if (dynA && dynB) {
            float invA = 1.0f / rbA->GetMass();
            float invB = 1.0f / rbB->GetMass();
            shareA = invA / (invA + invB);
            shareB = 1.0f - shareA;
        } else if (dynA) {
            shareA = 1.0f;
        } else if (dynB) {
            shareB = 1.0f;
        } else {
            shareA = shareB = 0.5f;
        }
    } else if (movA) {
        shareA = 1.0f;
    } else if (movB) {
        shareB = 1.0f;
    }
}

void Invoke(Component* c, bool trigger, ContactCallback kind,
            const CollisionEvent& event) {
    switch (kind) {
        case ContactCallback::Enter:
            trigger ? c->OnTriggerEnter(event) : c->OnCollisionEnter(event);
            break;
        case ContactCallback::Stay:
            trigger ? c->OnTriggerStay(event) : c->OnCollisionStay(event);
            break;
        case ContactCallback::Exit:
            trigger ? c->OnTriggerExit(event) : c->OnCollisionExit(event);
            break;
    }
}

}  // namespace

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

    // A callback is walking the pair lists; DispatchEvents purges them after
    if (m_dispatching) {
        m_removedDuringDispatch.push_back(collider);
        return;
    }

    auto involves = [collider](const TouchingPair& p) {
        return p.a == collider || p.b == collider;
    };
    m_prevTouching.erase(
        std::remove_if(m_prevTouching.begin(), m_prevTouching.end(), involves),
        m_prevTouching.end());
}

void PhysicsWorld::Step(float dt) {
    if (!(dt > 0.0f) || !std::isfinite(dt)) return;

    Integrate(dt);
    UpdateBroadphase();
    CollectPairs();
    ResolvePairs();
    DispatchEvents();
}

void PhysicsWorld::Integrate(float dt) {
    // We need wasGrounded BEFORE clearing, so gravity doesn't apply while standing
    for (auto* collider : m_colliders) {
        auto* rb = collider->GetRigidbody();
        if (!rb) continue;

        bool wasGrounded = rb->IsGrounded();
        rb->ClearCollisionState();

        if (rb->GetBodyType() != BodyType::Dynamic) continue;

        // Collision detection sets it again while still on the ground
        rb->SetGrounded(false);

        if (rb->GetUseGravity()) {
            Math::Vector3D vel = rb->GetVelocity();

            if (!wasGrounded) {
                // Airborne: apply full gravity
                vel = vel + rb->GetGravity() * dt;
            } else {
                // Grounded: a small downward velocity keeps collision
                // detection finding the floor without visible jitter
                if (vel.GetY() <= 0.0f) {
                    vel = Math::Vector3D(vel.GetX(), -0.5f, vel.GetZ());
                }
                // If vel.Y > 0 (jumping), leave it so the jump happens
            }

            // Clamp to terminal velocity
            float termVel = rb->GetTerminalVelocity();
            if (vel.GetY() < -termVel) {
                vel = Math::Vector3D(vel.GetX(), -termVel, vel.GetZ());
            }

            rb->SetVelocity(vel);
        }

        collider->TranslateOwner(rb->GetVelocity() * dt);
    }
}

void PhysicsWorld::UpdateBroadphase() {
    for (auto* collider : m_colliders) {
        int proxyId = collider->GetProxyId();
        if (proxyId < 0) continue;

        AABB newAABB = collider->GetWorldAABB();
        m_tree.MoveProxy(proxyId, newAABB, Vector3D(0, 0, 0));
    }
}

void PhysicsWorld::CollectPairs() {
    m_candidates.clear();

    for (auto* colliderA : m_colliders) {
        int proxyA = colliderA->GetProxyId();
        if (proxyA < 0) continue;

        bool movableA = IsMovable(colliderA->GetRigidbody());
        AABB worldA = colliderA->GetWorldAABB();

        m_tree.Query(worldA, [&](int proxyB) -> bool {
            if (proxyB <= proxyA) return true;

            auto* colliderB =
                static_cast<ColliderComponent*>(m_tree.GetUserData(proxyB));
            if (!movableA && !IsMovable(colliderB->GetRigidbody())) return true;

            if ((colliderA->GetLayer() & colliderB->GetMask()) == 0)
                return true;
            if ((colliderB->GetLayer() & colliderA->GetMask()) == 0)
                return true;

            TouchingPair pair;
            pair.a = colliderA;
            pair.b = colliderB;
            pair.key = PairKey(proxyA, proxyB);
            m_candidates.push_back(pair);
            return true;
        });
    }

    std::sort(m_candidates.begin(), m_candidates.end(),
              [](const TouchingPair& l, const TouchingPair& r) {
                  return l.key < r.key;
              });
}

void PhysicsWorld::ResolvePairs() {
    m_touching.clear();

    for (TouchingPair& pair : m_candidates) {
        ColliderComponent* colliderA = pair.a;
        ColliderComponent* colliderB = pair.b;

        // Fresh poses, so an earlier pair's correction is seen by this one
        Vector3D posA, scaleA, posB, scaleB;
        colliderA->GetWorldPose(posA, scaleA);
        colliderB->GetWorldPose(posB, scaleB);

        CollisionManifold manifold =
            TestCollision(colliderA->GetShape(), posA, scaleA,
                          colliderB->GetShape(), posB, scaleB);
        if (!manifold.hasCollision) continue;

        pair.trigger = colliderA->IsTrigger() || colliderB->IsTrigger();
        pair.contact = manifold.contact;
        m_touching.push_back(pair);

        if (pair.trigger) continue;

        auto* rbA = colliderA->GetRigidbody();
        auto* rbB = colliderB->GetRigidbody();

        float shareA = 0.0f, shareB = 0.0f;
        SeparationShares(rbA, rbB, shareA, shareB);

        const Vector3D& normal = manifold.contact.normal;
        float penetration = manifold.contact.penetration;

        if (IsMovable(rbA)) {
            if (shareA > 0.0f)
                colliderA->TranslateOwner(normal * (-penetration * shareA));
            rbA->ApplyContactNormal(normal * -1.0f);
        }
        if (IsMovable(rbB)) {
            if (shareB > 0.0f)
                colliderB->TranslateOwner(normal * (penetration * shareB));
            rbB->ApplyContactNormal(normal);
        }
    }
}

bool PhysicsWorld::RemovedDuringDispatch(
    const ColliderComponent* collider) const {
    return std::find(m_removedDuringDispatch.begin(),
                     m_removedDuringDispatch.end(),
                     collider) != m_removedDuringDispatch.end();
}

void PhysicsWorld::DispatchEvents() {
    m_dispatching = true;
    m_removedDuringDispatch.clear();

    // Stops as soon as a callback removes self's object, which deletes it
    auto notify = [this](ColliderComponent* self, bool trigger,
                         ContactCallback kind, const CollisionEvent& event) {
        GameObject* owner = self->GetOwner();
        if (!owner) return;
        const auto& components = owner->GetComponents();
        for (size_t i = 0; i < components.GetSize(); ++i) {
            if (Component* c = components[i].get())
                Invoke(c, trigger, kind, event);
            if (RemovedDuringDispatch(self)) return;
        }
    };

    auto send = [&](const TouchingPair& pair, ContactCallback kind) {
        if (RemovedDuringDispatch(pair.a) || RemovedDuringDispatch(pair.b))
            return;

        CollisionEvent forA;
        forA.self = pair.a;
        forA.other = pair.b;
        CollisionEvent forB;
        forB.self = pair.b;
        forB.other = pair.a;
        if (kind != ContactCallback::Exit) {
            forA.point = forB.point = pair.contact.point;
            forA.penetration = forB.penetration = pair.contact.penetration;
            forA.normal = pair.contact.normal;
            forB.normal = pair.contact.normal * -1.0f;
        }

        notify(pair.a, pair.trigger, kind, forA);
        if (RemovedDuringDispatch(pair.a) || RemovedDuringDispatch(pair.b))
            return;
        notify(pair.b, pair.trigger, kind, forB);
    };

    // Both lists are sorted by key, so one merge pass finds enter/stay/exit
    size_t prev = 0, cur = 0;
    while (prev < m_prevTouching.size() || cur < m_touching.size()) {
        if (cur == m_touching.size() ||
            (prev < m_prevTouching.size() &&
             m_prevTouching[prev].key < m_touching[cur].key)) {
            send(m_prevTouching[prev++], ContactCallback::Exit);
        } else if (prev == m_prevTouching.size() ||
                   m_touching[cur].key < m_prevTouching[prev].key) {
            send(m_touching[cur++], ContactCallback::Enter);
        } else {
            const TouchingPair& before = m_prevTouching[prev++];
            const TouchingPair& now = m_touching[cur++];
            if (before.trigger != now.trigger) {
                send(before, ContactCallback::Exit);
                send(now, ContactCallback::Enter);
            } else {
                send(now, ContactCallback::Stay);
            }
        }
    }

    m_dispatching = false;
    std::swap(m_prevTouching, m_touching);
    m_touching.clear();

    for (const ColliderComponent* removed : m_removedDuringDispatch) {
        m_prevTouching.erase(
            std::remove_if(m_prevTouching.begin(), m_prevTouching.end(),
                           [removed](const TouchingPair& p) {
                               return p.a == removed || p.b == removed;
                           }),
            m_prevTouching.end());
    }
    m_removedDuringDispatch.clear();
}

std::vector<OverlapHit> PhysicsWorld::OverlapShape(const ColliderShape& query,
                                                   const AABB& bounds,
                                                   uint32_t layerMask) const {
    std::vector<OverlapHit> results;
    const Vector3D origin(0, 0, 0);
    const Vector3D unitScale(1, 1, 1);

    m_tree.Query(bounds, [&](int proxyId) -> bool {
        auto* collider =
            static_cast<ColliderComponent*>(m_tree.GetUserData(proxyId));
        if ((collider->GetLayer() & layerMask) == 0) return true;

        Vector3D pos, scale;
        collider->GetWorldPose(pos, scale);
        CollisionManifold m = TestCollision(query, origin, unitScale,
                                            collider->GetShape(), pos, scale);
        if (!m.hasCollision) return true;

        OverlapHit hit;
        hit.collider = collider;
        hit.point = m.contact.point;
        hit.normal = m.contact.normal;
        hit.penetration = m.contact.penetration;
        results.push_back(hit);
        return true;
    });

    return results;
}

std::vector<OverlapHit> PhysicsWorld::OverlapSphere(const Vector3D& center,
                                                    float radius,
                                                    uint32_t layerMask) const {
    if (!(radius >= 0.0f)) return {};
    BoundingSphere sphere(center, radius);
    return OverlapShape(sphere, sphere.ToAABB(), layerMask);
}

std::vector<OverlapHit> PhysicsWorld::OverlapAABB(const AABB& aabb,
                                                  uint32_t layerMask) const {
    return OverlapShape(aabb, aabb, layerMask);
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
    RayHit result;

    float lenSq = direction.Dot(direction);
    if (!(lenSq > 1e-12f) || !(maxDist > 0.0f)) return result;
    Vector3D dir = direction * (1.0f / std::sqrt(lenSq));

    float closest = maxDist;

    m_tree.RayCast(origin, dir, maxDist, [&](int proxyId) -> bool {
        auto* collider =
            static_cast<ColliderComponent*>(m_tree.GetUserData(proxyId));
        if ((collider->GetLayer() & layerMask) == 0) return true;

        Vector3D pos, scale;
        collider->GetWorldPose(pos, scale);

        float t = 0.0f;
        Vector3D normal;
        if (!RaycastShape(collider->GetShape(), pos, scale, origin, dir,
                          closest, t, normal))
            return true;

        if (!result.hit || t < closest) {
            closest = t;
            result.hit = true;
            result.collider = collider;
            result.distance = t;
            result.point = origin + dir * t;
            result.normal = normal;
        }
        return true;
    });

    return result;
}

} // namespace Physics
} // namespace Sleak
