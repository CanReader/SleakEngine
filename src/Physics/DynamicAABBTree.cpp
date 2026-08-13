#include <Physics/DynamicAABBTree.hpp>
#include <stack>
#include <algorithm>
#include <cmath>

namespace Sleak {
namespace Physics {

DynamicAABBTree::DynamicAABBTree() {
    m_nodeCapacity = 16;
    m_nodes.resize(m_nodeCapacity);
    // Build free list
    for (int i = 0; i < m_nodeCapacity - 1; ++i) {
        m_nodes[i].parent = i + 1; // next free
        m_nodes[i].height = -1;
    }
    m_nodes[m_nodeCapacity - 1].parent = NULL_NODE;
    m_nodes[m_nodeCapacity - 1].height = -1;
    m_freeList = 0;
}

int DynamicAABBTree::AllocateNode() {
    if (m_freeList == NULL_NODE) {
        int oldCapacity = m_nodeCapacity;
        m_nodeCapacity *= 2;
        m_nodes.resize(m_nodeCapacity);

        for (int i = oldCapacity; i < m_nodeCapacity - 1; ++i) {
            m_nodes[i].parent = i + 1;
            m_nodes[i].height = -1;
        }
        m_nodes[m_nodeCapacity - 1].parent = NULL_NODE;
        m_nodes[m_nodeCapacity - 1].height = -1;
        m_freeList = oldCapacity;
    }

    int nodeId = m_freeList;
    m_freeList = m_nodes[nodeId].parent;

    m_nodes[nodeId].parent = NULL_NODE;
    m_nodes[nodeId].left = NULL_NODE;
    m_nodes[nodeId].right = NULL_NODE;
    m_nodes[nodeId].height = 0;
    m_nodes[nodeId].userData = nullptr;
    ++m_nodeCount;

    return nodeId;
}

void DynamicAABBTree::FreeNode(int nodeId) {
    m_nodes[nodeId].parent = m_freeList;
    m_nodes[nodeId].height = -1;
    m_nodes[nodeId].userData = nullptr;
    m_freeList = nodeId;
    --m_nodeCount;
}

int DynamicAABBTree::Insert(const AABB& aabb, void* userData) {
    int proxyId = AllocateNode();
    m_nodes[proxyId].fatAABB = aabb.Fatten(FAT_AABB_MARGIN);
    m_nodes[proxyId].userData = userData;
    m_nodes[proxyId].height = 0;

    InsertLeaf(proxyId);
    return proxyId;
}

void DynamicAABBTree::Remove(int proxyId) {
    RemoveLeaf(proxyId);
    FreeNode(proxyId);
}

bool DynamicAABBTree::MoveProxy(int proxyId, const AABB& newAABB, const Vector3D& displacement) {
    // If the new AABB is still inside the fat AABB, no need to update
    if (m_nodes[proxyId].fatAABB.Contains(newAABB.min) &&
        m_nodes[proxyId].fatAABB.Contains(newAABB.max)) {
        return false;
    }

    RemoveLeaf(proxyId);

    // Extend in direction of movement
    AABB fatAABB = newAABB.Fatten(FAT_AABB_MARGIN);
    Vector3D d = displacement * 2.0f;

    if (d.GetX() < 0.0f) {
        fatAABB.min.SetX(fatAABB.min.GetX() + d.GetX());
    } else {
        fatAABB.max.SetX(fatAABB.max.GetX() + d.GetX());
    }
    if (d.GetY() < 0.0f) {
        fatAABB.min.SetY(fatAABB.min.GetY() + d.GetY());
    } else {
        fatAABB.max.SetY(fatAABB.max.GetY() + d.GetY());
    }
    if (d.GetZ() < 0.0f) {
        fatAABB.min.SetZ(fatAABB.min.GetZ() + d.GetZ());
    } else {
        fatAABB.max.SetZ(fatAABB.max.GetZ() + d.GetZ());
    }

    m_nodes[proxyId].fatAABB = fatAABB;
    InsertLeaf(proxyId);
    return true;
}

void DynamicAABBTree::InsertLeaf(int leaf) {
    if (m_root == NULL_NODE) {
        m_root = leaf;
        m_nodes[m_root].parent = NULL_NODE;
        return;
    }

    // Find best sibling using Surface Area Heuristic
    AABB leafAABB = m_nodes[leaf].fatAABB;
    int index = m_root;

    while (!m_nodes[index].IsLeaf()) {
        int leftChild = m_nodes[index].left;
        int rightChild = m_nodes[index].right;

        float area = m_nodes[index].fatAABB.GetSurfaceArea();
        AABB combinedAABB = m_nodes[index].fatAABB.Merge(leafAABB);
        float combinedArea = combinedAABB.GetSurfaceArea();

        // Cost of creating a new parent
        float cost = 2.0f * combinedArea;
        float inheritanceCost = 2.0f * (combinedArea - area);

        // Cost of descending into left child
        float costLeft;
        if (m_nodes[leftChild].IsLeaf()) {
            costLeft = m_nodes[leftChild].fatAABB.Merge(leafAABB).GetSurfaceArea() + inheritanceCost;
        } else {
            float oldArea = m_nodes[leftChild].fatAABB.GetSurfaceArea();
            float newArea = m_nodes[leftChild].fatAABB.Merge(leafAABB).GetSurfaceArea();
            costLeft = (newArea - oldArea) + inheritanceCost;
        }

        // Cost of descending into right child
        float costRight;
        if (m_nodes[rightChild].IsLeaf()) {
            costRight = m_nodes[rightChild].fatAABB.Merge(leafAABB).GetSurfaceArea() + inheritanceCost;
        } else {
            float oldArea = m_nodes[rightChild].fatAABB.GetSurfaceArea();
            float newArea = m_nodes[rightChild].fatAABB.Merge(leafAABB).GetSurfaceArea();
            costRight = (newArea - oldArea) + inheritanceCost;
        }

        if (cost < costLeft && cost < costRight) break;

        index = (costLeft < costRight) ? leftChild : rightChild;
    }

    int sibling = index;

    // Create new parent
    int oldParent = m_nodes[sibling].parent;
    int newParent = AllocateNode();
    m_nodes[newParent].parent = oldParent;
    m_nodes[newParent].fatAABB = leafAABB.Merge(m_nodes[sibling].fatAABB);
    m_nodes[newParent].height = m_nodes[sibling].height + 1;

    if (oldParent != NULL_NODE) {
        if (m_nodes[oldParent].left == sibling) {
            m_nodes[oldParent].left = newParent;
        } else {
            m_nodes[oldParent].right = newParent;
        }
    } else {
        m_root = newParent;
    }

    m_nodes[newParent].left = sibling;
    m_nodes[newParent].right = leaf;
    m_nodes[sibling].parent = newParent;
    m_nodes[leaf].parent = newParent;

    // Walk back and fix heights and AABBs
    int node = m_nodes[leaf].parent;
    while (node != NULL_NODE) {
        node = Balance(node);

        int left = m_nodes[node].left;
        int right = m_nodes[node].right;

        m_nodes[node].height = 1 + std::max(m_nodes[left].height, m_nodes[right].height);
        m_nodes[node].fatAABB = m_nodes[left].fatAABB.Merge(m_nodes[right].fatAABB);

        node = m_nodes[node].parent;
    }
}

void DynamicAABBTree::RemoveLeaf(int leaf) {
    if (leaf == m_root) {
        m_root = NULL_NODE;
        return;
    }

    int parent = m_nodes[leaf].parent;
    int grandParent = m_nodes[parent].parent;
    int sibling = (m_nodes[parent].left == leaf) ? m_nodes[parent].right : m_nodes[parent].left;

    if (grandParent != NULL_NODE) {
        if (m_nodes[grandParent].left == parent) {
            m_nodes[grandParent].left = sibling;
        } else {
            m_nodes[grandParent].right = sibling;
        }
        m_nodes[sibling].parent = grandParent;
        FreeNode(parent);

        int node = grandParent;
        while (node != NULL_NODE) {
            node = Balance(node);

            int left = m_nodes[node].left;
            int right = m_nodes[node].right;

            m_nodes[node].fatAABB = m_nodes[left].fatAABB.Merge(m_nodes[right].fatAABB);
            m_nodes[node].height = 1 + std::max(m_nodes[left].height, m_nodes[right].height);

            node = m_nodes[node].parent;
        }
    } else {
        m_root = sibling;
        m_nodes[sibling].parent = NULL_NODE;
        FreeNode(parent);
    }
}

int DynamicAABBTree::Balance(int nodeId) {
    if (m_nodes[nodeId].IsLeaf() || m_nodes[nodeId].height < 2) {
        return nodeId;
    }

    int left = m_nodes[nodeId].left;
    int right = m_nodes[nodeId].right;

    int balance = m_nodes[right].height - m_nodes[left].height;

    // Rotate right branch up
    if (balance > 1) {
        int rightLeft = m_nodes[right].left;
        int rightRight = m_nodes[right].right;

        // Swap node and right child
        m_nodes[right].left = nodeId;
        m_nodes[right].parent = m_nodes[nodeId].parent;
        m_nodes[nodeId].parent = right;

        if (m_nodes[right].parent != NULL_NODE) {
            if (m_nodes[m_nodes[right].parent].left == nodeId) {
                m_nodes[m_nodes[right].parent].left = right;
            } else {
                m_nodes[m_nodes[right].parent].right = right;
            }
        } else {
            m_root = right;
        }

        // Rotate
        if (m_nodes[rightLeft].height > m_nodes[rightRight].height) {
            m_nodes[right].right = rightLeft;
            m_nodes[nodeId].right = rightRight;
            m_nodes[rightRight].parent = nodeId;
            m_nodes[nodeId].fatAABB = m_nodes[left].fatAABB.Merge(m_nodes[rightRight].fatAABB);
            m_nodes[right].fatAABB = m_nodes[nodeId].fatAABB.Merge(m_nodes[rightLeft].fatAABB);
            m_nodes[nodeId].height = 1 + std::max(m_nodes[left].height, m_nodes[rightRight].height);
            m_nodes[right].height = 1 + std::max(m_nodes[nodeId].height, m_nodes[rightLeft].height);
        } else {
            m_nodes[right].right = rightRight;
            m_nodes[nodeId].right = rightLeft;
            m_nodes[rightLeft].parent = nodeId;
            m_nodes[nodeId].fatAABB = m_nodes[left].fatAABB.Merge(m_nodes[rightLeft].fatAABB);
            m_nodes[right].fatAABB = m_nodes[nodeId].fatAABB.Merge(m_nodes[rightRight].fatAABB);
            m_nodes[nodeId].height = 1 + std::max(m_nodes[left].height, m_nodes[rightLeft].height);
            m_nodes[right].height = 1 + std::max(m_nodes[nodeId].height, m_nodes[rightRight].height);
        }

        return right;
    }

    // Rotate left branch up
    if (balance < -1) {
        int leftLeft = m_nodes[left].left;
        int leftRight = m_nodes[left].right;

        m_nodes[left].left = nodeId;
        m_nodes[left].parent = m_nodes[nodeId].parent;
        m_nodes[nodeId].parent = left;

        if (m_nodes[left].parent != NULL_NODE) {
            if (m_nodes[m_nodes[left].parent].left == nodeId) {
                m_nodes[m_nodes[left].parent].left = left;
            } else {
                m_nodes[m_nodes[left].parent].right = left;
            }
        } else {
            m_root = left;
        }

        if (m_nodes[leftLeft].height > m_nodes[leftRight].height) {
            m_nodes[left].right = leftLeft;
            m_nodes[nodeId].left = leftRight;
            m_nodes[leftRight].parent = nodeId;
            m_nodes[nodeId].fatAABB = m_nodes[right].fatAABB.Merge(m_nodes[leftRight].fatAABB);
            m_nodes[left].fatAABB = m_nodes[nodeId].fatAABB.Merge(m_nodes[leftLeft].fatAABB);
            m_nodes[nodeId].height = 1 + std::max(m_nodes[right].height, m_nodes[leftRight].height);
            m_nodes[left].height = 1 + std::max(m_nodes[nodeId].height, m_nodes[leftLeft].height);
        } else {
            m_nodes[left].right = leftRight;
            m_nodes[nodeId].left = leftLeft;
            m_nodes[leftLeft].parent = nodeId;
            m_nodes[nodeId].fatAABB = m_nodes[right].fatAABB.Merge(m_nodes[leftLeft].fatAABB);
            m_nodes[left].fatAABB = m_nodes[nodeId].fatAABB.Merge(m_nodes[leftRight].fatAABB);
            m_nodes[nodeId].height = 1 + std::max(m_nodes[right].height, m_nodes[leftLeft].height);
            m_nodes[left].height = 1 + std::max(m_nodes[nodeId].height, m_nodes[leftRight].height);
        }

        return left;
    }

    return nodeId;
}

void DynamicAABBTree::Query(const AABB& queryAABB, const std::function<bool(int)>& callback) const {
    if (m_root == NULL_NODE) return;

    std::stack<int> stack;
    stack.push(m_root);

    while (!stack.empty()) {
        int nodeId = stack.top();
        stack.pop();

        if (nodeId == NULL_NODE) continue;

        if (m_nodes[nodeId].fatAABB.Overlaps(queryAABB)) {
            if (m_nodes[nodeId].IsLeaf()) {
                bool shouldContinue = callback(nodeId);
                if (!shouldContinue) return;
            } else {
                stack.push(m_nodes[nodeId].left);
                stack.push(m_nodes[nodeId].right);
            }
        }
    }
}

void DynamicAABBTree::RayCast(const Vector3D& origin, const Vector3D& direction, float maxDist,
                               const std::function<bool(int)>& callback) const {
    if (m_root == NULL_NODE) return;

    Vector3D invDir(
        std::abs(direction.GetX()) > 1e-8f ? 1.0f / direction.GetX() : 1e8f,
        std::abs(direction.GetY()) > 1e-8f ? 1.0f / direction.GetY() : 1e8f,
        std::abs(direction.GetZ()) > 1e-8f ? 1.0f / direction.GetZ() : 1e8f
    );

    std::stack<int> stack;
    stack.push(m_root);

    while (!stack.empty()) {
        int nodeId = stack.top();
        stack.pop();

        if (nodeId == NULL_NODE) continue;

        const AABB& aabb = m_nodes[nodeId].fatAABB;

        // Ray-AABB slab test
        float tmin = 0.0f;
        float tmax = maxDist;

        for (int i = 0; i < 3; ++i) {
            float aabbMin, aabbMax, o, id;
            if (i == 0) { aabbMin = aabb.min.GetX(); aabbMax = aabb.max.GetX(); o = origin.GetX(); id = invDir.GetX(); }
            else if (i == 1) { aabbMin = aabb.min.GetY(); aabbMax = aabb.max.GetY(); o = origin.GetY(); id = invDir.GetY(); }
            else { aabbMin = aabb.min.GetZ(); aabbMax = aabb.max.GetZ(); o = origin.GetZ(); id = invDir.GetZ(); }

            float t1 = (aabbMin - o) * id;
            float t2 = (aabbMax - o) * id;

            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);

            if (tmin > tmax) break;
        }

        if (tmin > tmax) continue;

        if (m_nodes[nodeId].IsLeaf()) {
            if (!callback(nodeId)) return;
        } else {
            stack.push(m_nodes[nodeId].left);
            stack.push(m_nodes[nodeId].right);
        }
    }
}

} // namespace Physics
} // namespace Sleak
