#ifndef _AUDIOSYSTEM_HPP_
#define _AUDIOSYSTEM_HPP_

#include <Audio/AudioClip.hpp>
#include <Core/OSDef.hpp>
#include <Math/Vector.hpp>
#include <Memory/RefPtr.hpp>
#include <cstdint>
#include <mutex>
#include <vector>

struct SDL_AudioStream;

namespace Sleak {

class Camera;

/// Refers to one playing sound. Goes stale (every call becomes a no-op)
/// once the sound finishes, is stopped, or its voice is stolen.
/// @ingroup audio
struct AudioVoiceHandle {
    uint32_t index = 0;
    uint32_t generation = 0;
};

/// Software mixer on top of an SDL3 audio stream.
///
/// Application owns the one instance and reaches it through GetInstance().
/// Sounds play on a fixed number of voices; when all are busy the oldest
/// voice is stolen, preferring one that is not looping. Voices given a
/// position through SetVoicePosition() are spatialized against the
/// listener with inverse distance attenuation and constant power panning.
/// The listener follows the active camera unless SetListener() is called.
///
/// When no audio device can be opened (CI, headless servers) the system
/// logs a warning and every call becomes a no-op, so game code never has to
/// check. Call it from the main thread. Mixing runs on SDL's audio thread
/// and shares voice state with the main thread through an internal mutex.
///
/// @code{.cpp}
/// auto clip = Sleak::AudioClip::LoadWAV("assets/sounds/click.wav");
/// if (auto* audio = Sleak::AudioSystem::GetInstance())
///     audio->Play(clip, 0.8f);
/// @endcode
/// @see AudioClip, AudioSourceComponent
/// @ingroup audio
class ENGINE_API AudioSystem {
   public:
    /// Opens the default playback device. maxVoices caps simultaneous sounds.
    explicit AudioSystem(uint32_t maxVoices = 32);
    ~AudioSystem();

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    /// False when no device could be opened and every call is a no-op.
    bool IsEnabled() const { return m_stream != nullptr; }

    /// Starts clip at volume (linear, 1 = unchanged). Returns a stale
    /// handle when audio is disabled or the clip is unusable.
    AudioVoiceHandle Play(const RefPtr<AudioClip>& clip, float volume = 1.0f,
                          bool loop = false);
    /// Starts clip as a 3D sound at a world position (see SetVoicePosition).
    AudioVoiceHandle PlayAt(const RefPtr<AudioClip>& clip,
                            const Math::Vector3D& position, float volume = 1.0f,
                            bool loop = false, float minDistance = 1.0f,
                            float maxDistance = 50.0f);
    void Stop(AudioVoiceHandle voice);
    void StopAll();
    bool IsPlaying(AudioVoiceHandle voice) const;
    void SetVolume(AudioVoiceHandle voice, float volume);

    /// Makes the voice 3D at a world position. Sound is full volume inside
    /// minDistance and falls off as minDistance / distance up to maxDistance.
    void SetVoicePosition(AudioVoiceHandle voice,
                          const Math::Vector3D& position,
                          float minDistance = 1.0f, float maxDistance = 50.0f);

    void SetMasterVolume(float volume);
    float GetMasterVolume() const;

    /// Places the listener explicitly and stops following the camera.
    void SetListener(const Math::Vector3D& position,
                     const Math::Vector3D& forward, const Math::Vector3D& up);
    /// Makes the listener track the camera passed to Update() again.
    void SetListenerFollowsCamera(bool follow);

    /// Per-frame step from Application: releases finished voices and
    /// refreshes 3D gains from the listener.
    void Update(const Camera* activeCamera);

    /// Mixes frames of interleaved stereo float output. The device callback
    /// uses this; it is public so the mix can be rendered offline.
    void Mix(float* out, uint32_t frames);

    uint32_t GetSampleRate() const { return m_sampleRate; }
    uint32_t GetMaxVoices() const {
        return static_cast<uint32_t>(m_voices.size());
    }

    /// The system Application created, or null outside of an Application.
    static AudioSystem* GetInstance() { return Instance; }

   private:
    struct Voice {
        RefPtr<AudioClip> clip;
        const float* samples = nullptr;
        uint32_t frames = 0;
        uint32_t cursor = 0;
        uint32_t generation = 1;
        uint64_t startOrder = 0;
        float volume = 1.0f;
        bool inUse = false;
        bool playing = false;
        bool loop = false;
        bool spatial = false;
        bool fresh = true;
        Math::Vector3D position;
        float minDistance = 1.0f;
        float maxDistance = 50.0f;
        float targetLeft = 0.0f;
        float targetRight = 0.0f;
        float left = 0.0f;
        float right = 0.0f;
    };

    AudioVoiceHandle Start(const RefPtr<AudioClip>& clip, float volume,
                           bool loop, const Math::Vector3D* position,
                           float minDistance, float maxDistance);
    Voice* Resolve(AudioVoiceHandle voice);
    const Voice* Resolve(AudioVoiceHandle voice) const;
    void Release(Voice& voice);
    void UpdateGains(Voice& voice) const;

    static void StreamCallback(void* userdata, SDL_AudioStream* stream,
                               int additionalAmount, int totalAmount);

    mutable std::mutex m_mutex;
    std::vector<Voice> m_voices;
    std::vector<float> m_mixBuffer;
    SDL_AudioStream* m_stream = nullptr;
    uint32_t m_sampleRate = 48000;
    uint64_t m_playCounter = 0;
    float m_masterVolume = 1.0f;

    bool m_followCamera = true;
    Math::Vector3D m_listenerPosition;
    Math::Vector3D m_listenerForward;
    Math::Vector3D m_listenerUp;

    static AudioSystem* Instance;
};

}  // namespace Sleak

#endif  // _AUDIOSYSTEM_HPP_
