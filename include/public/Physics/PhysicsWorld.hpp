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
        struct CollisionPair {
            ColliderComponent* a = nullptr;
            ColliderComponent* b = nullptr;
            CollisionManifold manifold;
        };

        /// Result of a single Raycast call.
        struct RayHit {
            bool hit = false;
            ColliderComponent* collider = nullptr;
            Vector3D point;
            Vector3D normal;
            float distance = 0.0f;
        };

        /// Result of sweeping a moving shape (e.g. SphereSweep) against the world.
        struct SweepResult {
            bool hit = false;
            ColliderComponent* collider = nullptr;
            Vector3D point;
            Vector3D normal;
            float distance = 0.0f;
        };

        /// Owns the broadphase tree and every registered collider; drives collision detection and resolution each step.
        class PhysicsWorld {
        public:
            PhysicsWorld() = default;
            ~PhysicsWorld() = default;

            /// Advances the simulation by dt: applies gravity, updates the broadphase, and resolves overlaps.
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
