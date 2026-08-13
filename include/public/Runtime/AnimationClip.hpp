#ifndef _ANIMATIONCLIP_HPP_
#define _ANIMATIONCLIP_HPP_

#include <Core/OSDef.hpp>
#include <Math/Vector.hpp>
#include <Math/Quaternion.hpp>
#include <string>
#include <vector>
#include <unordered_map>

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
            auto it = m_channelLookup.find(boneName);
            return (it != m_channelLookup.end()) ? &channels[it->second] : nullptr;
        }

        // Call after all channels have been added
        void BuildLookup() {
            m_channelLookup.clear();
            for (size_t i = 0; i < channels.size(); ++i) {
                m_channelLookup[channels[i].boneName] = static_cast<int>(i);
            }
        }

    private:
        std::unordered_map<std::string, int> m_channelLookup;
    };

} // namespace Sleak

#endif // _ANIMATIONCLIP_HPP_
