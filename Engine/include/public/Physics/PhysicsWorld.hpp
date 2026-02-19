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

        struct CollisionPair {
            ColliderComponent* a = nullptr;
            ColliderComponent* b = nullptr;
            CollisionManifold manifold;
        };

        struct RayHit {
            bool hit = false;
            ColliderComponent* collider = nullptr;
            Vector3D point;
            Vector3D normal;
            float distance = 0.0f;
        };

        struct SweepResult {
            bool hit = false;
            ColliderComponent* collider = nullptr;
            Vector3D point;
            Vector3D normal;
            float distance = 0.0f;
        };

        class PhysicsWorld {
        public:
            PhysicsWorld() = default;
            ~PhysicsWorld() = default;

            void Step(float dt);

            void RegisterCollider(ColliderComponent* collider);
            void UnregisterCollider(ColliderComponent* collider);

            // Query API
            std::vector<CollisionPair> OverlapSphere(const Vector3D& center, float radius, uint32_t layerMask = 0xFFFFFFFF) const;
            std::vector<CollisionPair> OverlapAABB(const AABB& aabb, uint32_t layerMask = 0xFFFFFFFF) const;
            SweepResult SphereSweep(const Vector3D& start, const Vector3D& direction,
                                    float radius, float maxDist, uint32_t layerMask = 0xFFFFFFFF) const;
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
