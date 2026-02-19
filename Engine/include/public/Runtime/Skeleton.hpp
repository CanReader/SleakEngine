#ifndef _SKELETON_HPP_
#define _SKELETON_HPP_

#include <Core/OSDef.hpp>
#include <Math/Matrix.hpp>
#include <string>
#include <vector>
#include <unordered_map>

namespace Sleak {

    static constexpr int MAX_BONES = 256;
    static constexpr int MAX_BONE_INFLUENCE = 4;

    struct Bone {
        std::string name;
        int id;
        int parentId = -1;
        Math::Matrix4 offsetMatrix; // mesh space -> bone space
    };

    // Full scene node (bones + non-bone nodes)
    struct NodeData {
        std::string name;
        Math::Matrix4 defaultTransform; // node's local transform from scene graph
        int boneIndex = -1;             // index into bone array, -1 if not a bone
        std::vector<int> children;      // indices into node array
    };

    class ENGINE_API Skeleton {
    public:
        Skeleton() : m_globalInverseTransform(Math::Matrix4::Identity()) {}

        int GetBoneCount() const { return static_cast<int>(m_bones.size()); }

        int FindBoneId(const std::string& name) const {
            auto it = m_boneNameToId.find(name);
            return (it != m_boneNameToId.end()) ? it->second : -1;
        }

        const Bone& GetBone(int id) const { return m_bones[id]; }

        int AddBone(const Bone& bone) {
            int id = static_cast<int>(m_bones.size());
            Bone b = bone;
            b.id = id;
            m_boneNameToId[b.name] = id;
            m_bones.push_back(b);
            return id;
        }

        const Math::Matrix4& GetGlobalInverseTransform() const {
            return m_globalInverseTransform;
        }

        void SetGlobalInverseTransform(const Math::Matrix4& mat) {
            m_globalInverseTransform = mat;
        }

        // --- Full node tree ---

        int AddNode(const NodeData& node) {
            int idx = static_cast<int>(m_nodes.size());
            m_nodes.push_back(node);
            m_nodeNameToIndex[node.name] = idx;
            return idx;
        }

        void AddNodeChild(int parentIdx, int childIdx) {
            if (parentIdx >= 0 && parentIdx < static_cast<int>(m_nodes.size()))
                m_nodes[parentIdx].children.push_back(childIdx);
        }

        int GetNodeCount() const { return static_cast<int>(m_nodes.size()); }
        const NodeData& GetNode(int idx) const { return m_nodes[idx]; }

        int GetRootNodeIndex() const { return m_nodes.empty() ? -1 : 0; }

        int FindNodeIndex(const std::string& name) const {
            auto it = m_nodeNameToIndex.find(name);
            return (it != m_nodeNameToIndex.end()) ? it->second : -1;
        }

        bool HasNodeTree() const { return !m_nodes.empty(); }

        void SetBoneParent(int boneId, int parentId) {
            if (boneId >= 0 && boneId < static_cast<int>(m_bones.size()))
                m_bones[boneId].parentId = parentId;
        }

    private:
        std::vector<Bone> m_bones;
        std::unordered_map<std::string, int> m_boneNameToId;
        Math::Matrix4 m_globalInverseTransform;

        // Full scene node tree
        std::vector<NodeData> m_nodes;
        std::unordered_map<std::string, int> m_nodeNameToIndex;
    };

} // namespace Sleak

#endif // _SKELETON_HPP_
