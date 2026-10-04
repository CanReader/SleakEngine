#ifndef _AUDIOSOURCECOMPONENT_HPP_
#define _AUDIOSOURCECOMPONENT_HPP_

#include <Audio/AudioClip.hpp>
#include <Audio/AudioSystem.hpp>
#include <ECS/Component.hpp>
#include <Memory/RefPtr.hpp>

namespace Sleak {

class TransformComponent;

/// Plays an AudioClip from a GameObject. Spatial sources follow the
/// owner's TransformComponent every frame.
///
/// With playOnEnable set the clip starts when the object is initialized
/// active and again whenever it is re-enabled; disabling or destroying the
/// object stops it.
///
/// @code{.cpp}
/// auto clip = Sleak::AudioClip::LoadWAV("assets/sounds/water.wav");
/// obj->AddComponent<Sleak::AudioSourceComponent>(clip, 0.6f, true, true);
/// @endcode
/// @see AudioSystem, AudioClip
/// @ingroup audio
class ENGINE_API AudioSourceComponent : public Component {
   public:
    AudioSourceComponent(GameObject* owner, RefPtr<AudioClip> clip,
                         float volume = 1.0f, bool loop = false,
                         bool playOnEnable = false, bool spatial = true);

    bool Initialize() override;
    /// Moves a spatial voice to the owner's world position.
    void Update(float deltaTime) override;
    void OnEnable() override;
    void OnDisable() override;
    void OnDestroy() override;

    /// Restarts the clip from the beginning.
    void Play();
    void Stop();
    bool IsPlaying() const;

    void SetClip(RefPtr<AudioClip> clip) { m_clip = std::move(clip); }
    void SetVolume(float volume);
    float GetVolume() const { return m_volume; }
    void SetLoop(bool loop) { m_loop = loop; }
    void SetPlayOnEnable(bool playOnEnable) { m_playOnEnable = playOnEnable; }
    /// Full volume inside minDistance, no further falloff past maxDistance.
    void SetDistances(float minDistance, float maxDistance);

   private:
    void PushPosition();

    RefPtr<AudioClip> m_clip;
    AudioVoiceHandle m_voice;
    TransformComponent* m_transform = nullptr;
    float m_volume;
    float m_minDistance = 1.0f;
    float m_maxDistance = 50.0f;
    bool m_loop;
    bool m_playOnEnable;
    bool m_spatial;
};

}  // namespace Sleak

#endif  // _AUDIOSOURCECOMPONENT_HPP_
