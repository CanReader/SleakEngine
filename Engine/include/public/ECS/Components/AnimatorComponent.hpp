#ifndef _ANIMATORCOMPONENT_HPP_
#define _ANIMATORCOMPONENT_HPP_

#include <ECS/Component.hpp>
#include <Runtime/Skeleton.hpp>
#include <Runtime/AnimationClip.hpp>
#include <Memory/RefPtr.h>
#include <Math/Matrix.hpp>
#include <vector>
#include <string>

namespace Sleak {

    namespace RenderEngine {
        class BufferBase;
    }

    class ENGINE_API AnimatorComponent : public Component {
    public:
        AnimatorComponent(GameObject* owner, Skeleton* skeleton,
                          std::vector<AnimationClip*> clips);
        virtual ~AnimatorComponent();

        virtual bool Initialize() override;
        virtual void Update(float deltaTime) override;

        // Animation control
        void Play(const std::string& clipName, bool loop = true);
        void Play(int clipIndex, bool loop = true);
        void Stop();
        void Pause();
        void Resume();
        void SetSpeed(float speed);
        float GetSpeed() const;
        bool IsPlaying() const;
        float GetCurrentTime() const;
        const std::string& GetCurrentClipName() const;

        // Get the bone matrix buffer for MeshComponent attachment
        RefPtr<RenderEngine::BufferBase> GetBoneBuffer() const;

    private:
        void ComputeBoneTransforms(float animTime);
        void ProcessNodeHierarchy(int nodeIndex, const Math::Matrix4& parentTransform,
                                  float animTime);

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
        RefPtr<RenderEngine::BufferBase> m_boneBuffer;
    };

} // namespace Sleak

#endif // _ANIMATORCOMPONENT_HPP_
