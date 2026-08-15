#ifndef _ANIMATORCOMPONENT_HPP_
#define _ANIMATORCOMPONENT_HPP_

#include <ECS/Component.hpp>
#include <Runtime/Skeleton.hpp>
#include <Runtime/AnimationClip.hpp>
#include <Memory/RefPtr.hpp>
#include <Math/Matrix.hpp>
#include <vector>
#include <string>

namespace Sleak {

    namespace RenderEngine {
        class BufferBase;
    }

    class AnimationStateMachine;

    /// Samples a skeleton's bones from clips or a state machine and uploads
    /// the result to the sibling MeshComponent's bone buffer each frame.
    /// @ingroup scene
    class ENGINE_API AnimatorComponent : public Component {
    public:
        AnimatorComponent(GameObject* owner, Skeleton* skeleton,
                          std::vector<AnimationClip*> clips);
        virtual ~AnimatorComponent();

        /// Allocates the bone constant buffer and attaches it to the sibling MeshComponent.
        virtual bool Initialize() override;
        /// Samples the active state machine or clip and pushes the resulting bone matrices to the GPU.
        virtual void Update(float deltaTime) override;

        // Animation control
        /// Switches to the clip by name, restarting from time zero.
        void Play(const std::string& clipName, bool loop = true);
        /// Switches to the clip by index, restarting from time zero.
        void Play(int clipIndex, bool loop = true);
        void Stop();
        void Pause();
        void Resume();
        void SetSpeed(float speed);
        float GetSpeed() const;
        bool IsPlaying() const;
        float GetCurrentTime() const;
        const std::string& GetCurrentClipName() const;

        // State machine
        /// Replaces any existing state machine with a fresh, empty one.
        AnimationStateMachine* CreateStateMachine();
        AnimationStateMachine* GetStateMachine() const { return m_stateMachine; }

        // Clip management
        void AddClip(AnimationClip* clip);
        Skeleton* GetSkeleton() const { return m_skeleton; }
        const std::vector<AnimationClip*>& GetClips() const { return m_clips; }

        // Get the bone matrix buffer for MeshComponent attachment
        RefPtr<RenderEngine::BufferBase> GetBoneBuffer() const;

    private:
        // Compute bone transforms for a specific clip/time into output buffer
        /// Walks the skeleton's node tree for clip at animTime, writing final bone matrices to outMatrices.
        void ComputeBoneTransformsForClip(AnimationClip* clip, float animTime,
                                          std::vector<Math::Matrix4>& outMatrices);
        /// Recursive step of ComputeBoneTransformsForClip, propagating parentTransform down the hierarchy.
        void ProcessNodeHierarchyForClip(int nodeIndex, const Math::Matrix4& parentTransform,
                                          AnimationClip* clip, float animTime,
                                          std::vector<Math::Matrix4>& outMatrices);

        // Legacy single-clip path
        /// Same as ComputeBoneTransformsForClip but samples m_clips[m_currentClip] into m_boneMatrices.
        void ComputeBoneTransforms(float animTime);
        /// Recursive step of ComputeBoneTransforms.
        void ProcessNodeHierarchy(int nodeIndex, const Math::Matrix4& parentTransform,
                                  float animTime);

        // Blend two sets of bone matrices
        /// Linearly interpolates each matrix element between a and b, storing the result in out.
        static void BlendBoneMatrices(const std::vector<Math::Matrix4>& a,
                                      const std::vector<Math::Matrix4>& b,
                                      float weight,
                                      std::vector<Math::Matrix4>& out);

        // Keyframe interpolation
        Math::Vector3D InterpolatePosition(const AnimationChannel& channel, float time);
        Math::Quaternion InterpolateRotation(const AnimationChannel& channel, float time);
        Math::Vector3D InterpolateScale(const AnimationChannel& channel, float time);

        // Quaternion slerp (engine doesn't have one)
        static Math::Quaternion Slerp(const Math::Quaternion& a, const Math::Quaternion& b, float t);

        Skeleton* m_skeleton;
        std::vector<AnimationClip*> m_clips;

        int m_currentClip = -1;
        float m_currentTime = 0.0f;
        float m_speed = 1.0f;
        bool m_playing = false;
        bool m_loop = true;

        // Final bone matrices: finalMatrix[i] = globalInverse * globalTransform[i] * offsetMatrix[i]
        std::vector<Math::Matrix4> m_boneMatrices;
        std::vector<Math::Matrix4> m_boneMatricesB;  // second pose for blending
        RefPtr<RenderEngine::BufferBase> m_boneBuffer;

        // State machine (owned)
        AnimationStateMachine* m_stateMachine = nullptr;
    };

} // namespace Sleak

#endif // _ANIMATORCOMPONENT_HPP_
