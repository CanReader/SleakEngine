#ifndef _ANIMATIONCLIP_HPP_
#define _ANIMATIONCLIP_HPP_

#include <Core/OSDef.hpp>
#include <Math/Vector.hpp>
#include <Math/Quaternion.hpp>
#include <string>
#include <vector>

namespace Sleak {

    template<typename T>
    struct Keyframe {
        float time;
        T value;
    };

    struct AnimationChannel {
        std::string boneName;
        int boneId = -1;
        std::vector<Keyframe<Math::Vector3D>> positionKeys;
        std::vector<Keyframe<Math::Quaternion>> rotationKeys;
        std::vector<Keyframe<Math::Vector3D>> scaleKeys;
    };

    class ENGINE_API AnimationClip {
    public:
        std::string name;
        float duration = 0.0f;       // in ticks
        float ticksPerSecond = 25.0f; // ticks per second
        std::vector<AnimationChannel> channels;

        float GetDurationInSeconds() const {
            return (ticksPerSecond > 0.0f) ? duration / ticksPerSecond : 0.0f;
        }

        const AnimationChannel* FindChannel(const std::string& boneName) const {
            for (const auto& ch : channels) {
                if (ch.boneName == boneName)
                    return &ch;
            }
            return nullptr;
        }
    };

} // namespace Sleak

#endif // _ANIMATIONCLIP_HPP_
