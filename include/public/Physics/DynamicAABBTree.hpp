#ifndef _DYNAMIC_AABB_TREE_HPP_
#define _DYNAMIC_AABB_TREE_HPP_

#include <Physics/Colliders.hpp>
#include <functional>
#include <vector>

namespace Sleak {
namespace Physics {

    static constexpr int NULL_NODE = -1;
    static constexpr float FAT_AABB_MARGIN = 0.1f;

    /// One node of the DynamicAABBTree, either an internal node or a leaf holding a proxy's fattened AABB.
    struct TreeNode {
        AABB fatAABB;
        void* userData = nullptr;
        int parent = NULL_NODE;
        int left = NULL_NODE;
        int right = NULL_NODE;
        int height = 0;

        bool IsLeaf() const { return left == NULL_NODE; }
    };

    /// Broadphase AABB tree; PhysicsWorld inserts colliders as proxies and queries overlaps against it.
    class DynamicAABBTree {
    public:
        DynamicAABBTree();
        ~DynamicAABBTree() = default;

        /// Adds a new proxy with a fattened AABB and returns its id.
        int Insert(const AABB& aabb, void* userData);
        /// Removes a proxy and rebalances the tree around it.
        void Remove(int proxyId);
        /// Refits a proxy's fat AABB to newAABB, re-inserting it only if it moved outside the fat margin.
        bool MoveProxy(int proxyId, const AABB& newAABB, const Vector3D& displacement);

        /// Visits every leaf whose fat AABB overlaps queryAABB; stop early by returning false from callback.
        void Query(const AABB& queryAABB, const std::function<bool(int)>& callback) const;
        /// Walks the tree along a ray, visiting candidate leaves within maxDist.
        void RayCast(const Vector3D& origin, const Vector3D& direction, float maxDist,
                     const std::function<bool(int)>& callback) const;

        const AABB& GetFatAABB(int proxyId) const { return m_nodes[proxyId].fatAABB; }
        void* GetUserData(int proxyId) const { return m_nodes[proxyId].userData; }

    private:
        int AllocateNode();
        void FreeNode(int nodeId);
        void InsertLeaf(int leaf);
        void RemoveLeaf(int leaf);
        int Balance(int nodeId);

        std::vector<TreeNode> m_nodes;
        int m_root = NULL_NODE;
        int m_freeList = NULL_NODE;
        int m_nodeCount = 0;
        int m_nodeCapacity = 0;
    };

} // namespace Physics
} // namespace Sleak

#endif // _DYNAMIC_AABB_TREE_HPP_
