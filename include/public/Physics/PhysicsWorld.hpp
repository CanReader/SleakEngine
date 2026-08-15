#ifndef _PHYSICS_WORLD_HPP_
#define _PHYSICS_WORLD_HPP_

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

        /// Owns the broadphase tree and every registered collider; drives collision detection and resolution each step.
        ///
        /// Each Scene creates one PhysicsWorld and steps it from its
        /// update, so you normally reach it with
        /// SceneBase::GetPhysicsWorld() rather than constructing one. You
        /// also rarely call RegisterCollider() by hand: adding a
        /// GameObject that carries a ColliderComponent registers it (and
        /// its children) automatically.
        ///
        /// Step() integrates every dynamic RigidbodyComponent using that
        /// body's own gravity setting, refreshes the DynamicAABBTree
        /// broadphase, then finds and resolves overlapping pairs.
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
        /// for (const auto& pair : caught) {
        ///     ApplyDamage(pair.b->GetOwner());
        /// }
        /// @endcode
        ///
        /// @see ColliderComponent, RigidbodyComponent, DynamicAABBTree,
        ///      RayHit, SweepResult, CollisionPair
        /// @ingroup physics
        class PhysicsWorld {
        public:
            PhysicsWorld() = default;
            ~PhysicsWorld() = default;

            /// Advances the simulation by dt: integrates each dynamic rigidbody's
            /// velocity, including its own gravity setting, into position, then
            /// updates the broadphase and resolves collisions.
            void Step(float dt);

            void RegisterCollider(ColliderComponent* collider);
            void UnregisterCollider(ColliderComponent* collider);

            /// Query API: colliders overlapping a sphere, filtered by layerMask.
            std::vector<CollisionPair> OverlapSphere(const Vector3D& center, float radius, uint32_t layerMask = 0xFFFFFFFF) const;
            /// Colliders overlapping an AABB, filtered by layerMask.
            std::vector<CollisionPair> OverlapAABB(const AABB& aabb, uint32_t layerMask = 0xFFFFFFFF) const;
            /// Sweeps a sphere from start along direction and returns the first collider it hits within maxDist.
            SweepResult SphereSweep(const Vector3D& start, const Vector3D& direction,
                                    float radius, float maxDist, uint32_t layerMask = 0xFFFFFFFF) const;
            /// Casts a ray and returns the closest collider hit within maxDist.
            RayHit Raycast(const Vector3D& origin, const Vector3D& direction,
                           float maxDist, uint32_t layerMask = 0xFFFFFFFF) const;

        private:
            void UpdateBroadphase();
            void FindPairsAndResolve();

            DynamicAABBTree m_tree;
            std::vector<ColliderComponent*> m_colliders;
        };

    } // namespace Physics
} // namespace Sleak

#endif // _PHYSICS_WORLD_HPP_
