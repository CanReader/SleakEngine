#ifndef _PHYSICS_WORLD_HPP_
#define _PHYSICS_WORLD_HPP_

#include <Core/OSDef.hpp>
#include <Physics/Colliders.hpp>
#include <Physics/CollisionDetection.hpp>
#include <Physics/DynamicAABBTree.hpp>
#include <vector>
#include <cstdint>

namespace Sleak {

    class ColliderComponent;

    namespace Physics {

        /// Two overlapping colliders and the manifold describing how they overlap.
        /// @ingroup physics
        struct CollisionPair {
            ColliderComponent* a = nullptr;
            ColliderComponent* b = nullptr;
            CollisionManifold manifold;
        };

        /// One collider found by OverlapSphere() or OverlapAABB().
        /// @ingroup physics
        struct OverlapHit {
            ColliderComponent* collider = nullptr;
            Vector3D point;
            /// Points from the query shape toward the collider.
            Vector3D normal;
            float penetration = 0.0f;
        };

        /// What a component receives in OnCollision* and OnTrigger* callbacks.
        /// Point, normal, and penetration are zero on Exit.
        /// @ingroup physics
        struct CollisionEvent {
            /// The collider on the object receiving the callback.
            ColliderComponent* self = nullptr;
            ColliderComponent* other = nullptr;
            Vector3D point;
            /// Points from self toward other.
            Vector3D normal;
            float penetration = 0.0f;
        };

        /// Result of a single Raycast call.
        /// @ingroup physics
        struct RayHit {
            bool hit = false;
            ColliderComponent* collider = nullptr;
            Vector3D point;
            Vector3D normal;
            float distance = 0.0f;
        };

        /// Result of sweeping a moving shape (e.g. SphereSweep) against the world.
        /// @ingroup physics
        struct SweepResult {
            bool hit = false;
            ColliderComponent* collider = nullptr;
            Vector3D point;
            Vector3D normal;
            float distance = 0.0f;
        };

        /// Owns the broadphase tree and every registered collider; drives
        /// collision detection and resolution each step.
        ///
        /// Each Scene creates one PhysicsWorld and steps it from its
        /// FixedUpdate() on the scene's fixed timestep, so you normally reach
        /// it with SceneBase::GetPhysicsWorld() rather than constructing one.
        /// You also rarely call RegisterCollider() by hand: adding a GameObject
        /// that carries a ColliderComponent registers it (and its children)
        /// automatically.
        ///
        /// Step() integrates every dynamic RigidbodyComponent using that
        /// body's own gravity setting, refreshes the DynamicAABBTree
        /// broadphase, then finds and resolves overlapping pairs. Only pairs
        /// where at least one side has a non-static rigidbody are tested.
        /// Two dynamic bodies split the correction by inverse mass, a
        /// kinematic body pushes a dynamic one without being pushed, and a
        /// trigger never pushes anything.
        ///
        /// After resolving, the world compares this step's touching pairs
        /// with the last step's and calls OnCollisionEnter/Stay/Exit, or
        /// OnTriggerEnter/Stay/Exit when either side is a trigger, on every
        /// component of both objects. Pairs involving a collider that gets
        /// unregistered (its object removed or destroyed) end without an Exit.
        ///
        /// The part you will use directly is the query API. Raycast(),
        /// SphereSweep(), OverlapSphere(), and OverlapAABB() all take an
        /// optional `layerMask` that is matched against
        /// ColliderComponent::GetLayer(), so you can aim a query at exactly
        /// the kind of object you care about.
        ///
        /// @code{.cpp}
        /// auto* world = GetPhysicsWorld();
        /// if (!world) return;
        ///
        /// // What is the player looking at, within 5 meters?
        /// Sleak::Physics::RayHit hit = world->Raycast(
        ///     camera->GetPosition(), camera->GetDirection(), 5.0f);
        /// if (hit.hit) {
        ///     SLEAK_INFO("Hit {} at {}m",
        ///                hit.collider->GetOwner()->GetName(), hit.distance);
        /// }
        ///
        /// // Everything inside a blast radius
        /// auto caught = world->OverlapSphere(
        ///     Sleak::Math::Vector3D(0.0f, 1.0f, 0.0f), 4.0f);
        /// for (const auto& hit : caught) {
        ///     ApplyDamage(hit.collider->GetOwner());
        /// }
        /// @endcode
        ///
        /// @see ColliderComponent, RigidbodyComponent, DynamicAABBTree,
        ///      RayHit, SweepResult, OverlapHit, CollisionEvent
        /// @ingroup physics
        class ENGINE_API PhysicsWorld {
        public:
            PhysicsWorld() = default;
            ~PhysicsWorld() = default;

            /// Advances the simulation by dt: integrates each dynamic
            /// rigidbody's velocity, including its own gravity setting, into
            /// position, then updates the broadphase and resolves collisions.
            /// Ignores dt <= 0.
            void Step(float dt);

            void RegisterCollider(ColliderComponent* collider);
            void UnregisterCollider(ColliderComponent* collider);

            /// Query API: colliders whose shape overlaps a sphere, filtered
            /// by layerMask.
            std::vector<OverlapHit> OverlapSphere(
                const Vector3D& center, float radius,
                uint32_t layerMask = 0xFFFFFFFF) const;
            /// Colliders whose shape overlaps an AABB, filtered by layerMask.
            std::vector<OverlapHit> OverlapAABB(
                const AABB& aabb, uint32_t layerMask = 0xFFFFFFFF) const;
            /// Sweeps a sphere from start along direction and returns the first collider it hits within maxDist.
            SweepResult SphereSweep(const Vector3D& start, const Vector3D& direction,
                                    float radius, float maxDist, uint32_t layerMask = 0xFFFFFFFF) const;
            /// Casts a ray against the collider shapes and returns the closest
            /// hit within maxDist. direction need not be normalized, distance
            /// is in world units, and colliders containing origin are skipped.
            RayHit Raycast(const Vector3D& origin, const Vector3D& direction,
                           float maxDist, uint32_t layerMask = 0xFFFFFFFF) const;

        private:
            struct TouchingPair {
                ColliderComponent* a = nullptr;
                ColliderComponent* b = nullptr;
                uint64_t key = 0;
                bool trigger = false;
                ContactPoint contact;
            };

            std::vector<OverlapHit> OverlapShape(const ColliderShape& query,
                                                 const AABB& bounds,
                                                 uint32_t layerMask) const;
            void Integrate(float dt);
            void UpdateBroadphase();
            void CollectPairs();
            void ResolvePairs();
            void DispatchEvents();
            bool RemovedDuringDispatch(const ColliderComponent* collider) const;

            DynamicAABBTree m_tree;
            std::vector<ColliderComponent*> m_colliders;

            std::vector<TouchingPair> m_candidates;
            std::vector<TouchingPair> m_touching;
            std::vector<TouchingPair> m_prevTouching;
            std::vector<const ColliderComponent*> m_removedDuringDispatch;
            bool m_dispatching = false;
        };

    } // namespace Physics
} // namespace Sleak

#endif // _PHYSICS_WORLD_HPP_
