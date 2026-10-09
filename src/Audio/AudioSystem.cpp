#include <SDL3/SDL.h>

#include <Audio/AudioSystem.hpp>
#include <Camera/Camera.hpp>
#include <Core/Logger.hpp>
#include <algorithm>
#include <cmath>

namespace Sleak {

AudioSystem* AudioSystem::Instance = nullptr;

namespace {
constexpr uint32_t kMixChunkFrames = 1024;
constexpr float kHalfPi = 1.57079632679f;
}  // namespace

AudioSystem::AudioSystem(uint32_t maxVoices)
    : m_voices(maxVoices),
      m_listenerForward(0.0f, 0.0f, 1.0f),
      m_listenerUp(0.0f, 1.0f, 0.0f) {
    if (!Instance) Instance = this;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SLEAK_WARN("Audio disabled, SDL audio init failed: {}", SDL_GetError());
        return;
    }

    SDL_AudioSpec deviceSpec{};
    int deviceFrames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &deviceSpec,
                                 &deviceFrames) &&
        deviceSpec.freq > 0)
        m_sampleRate = static_cast<uint32_t>(deviceSpec.freq);

    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, static_cast<int>(m_sampleRate)};
    m_mixBuffer.resize(kMixChunkFrames * 2);
    m_stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                  &AudioSystem::StreamCallback, this);
    if (!m_stream) {
        SLEAK_WARN("Audio disabled, no playback device: {}", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return;
    }

    SDL_ResumeAudioStreamDevice(m_stream);
    SLEAK_INFO("Audio started on '{}' at {} Hz, {} voices",
               SDL_GetCurrentAudioDriver(), m_sampleRate, maxVoices);
}

AudioSystem::~AudioSystem() {
    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    m_voices.clear();
    if (Instance == this) Instance = nullptr;
}

AudioSystem::Voice* AudioSystem::Resolve(AudioVoiceHandle voice) {
    if (voice.index >= m_voices.size()) return nullptr;
    Voice& v = m_voices[voice.index];
    return v.inUse && v.generation == voice.generation ? &v : nullptr;
}

const AudioSystem::Voice* AudioSystem::Resolve(AudioVoiceHandle voice) const {
    return const_cast<AudioSystem*>(this)->Resolve(voice);
}

void AudioSystem::Release(Voice& voice) {
    voice.clip = RefPtr<AudioClip>();
    voice.samples = nullptr;
    voice.inUse = false;
    voice.playing = false;
    if (++voice.generation == 0) voice.generation = 1;
}

void AudioSystem::UpdateGains(Voice& voice) const {
    if (!voice.spatial) {
        voice.targetLeft = voice.targetRight = voice.volume;
        return;
    }

    const Math::Vector3D toSource = voice.position - m_listenerPosition;
    const float distance = toSource.Magnitude();
    const float minD = std::max(voice.minDistance, 0.001f);
    const float maxD = std::max(voice.maxDistance, minD);
    const float attenuation = minD / std::clamp(distance, minD, maxD);

    float pan = 0.0f;
    if (distance > 1e-4f) {
        Math::Vector3D right = m_listenerUp.Cross(m_listenerForward);
        const float rightLen = right.Magnitude();
        if (rightLen > 1e-6f)
            pan = std::clamp(toSource.Dot(right) / (rightLen * distance), -1.0f,
                             1.0f);
    }

    const float angle = (pan + 1.0f) * 0.5f * kHalfPi;
    const float gain = voice.volume * attenuation;
    voice.targetLeft = gain * std::cos(angle);
    voice.targetRight = gain * std::sin(angle);
}

AudioVoiceHandle AudioSystem::Play(const RefPtr<AudioClip>& clip, float volume,
                                   bool loop) {
    return Start(clip, volume, loop, nullptr, 1.0f, 50.0f);
}

AudioVoiceHandle AudioSystem::PlayAt(const RefPtr<AudioClip>& clip,
                                     const Math::Vector3D& position,
                                     float volume, bool loop, float minDistance,
                                     float maxDistance) {
    return Start(clip, volume, loop, &position, minDistance, maxDistance);
}

AudioVoiceHandle AudioSystem::Start(const RefPtr<AudioClip>& clip, float volume,
                                    bool loop, const Math::Vector3D* position,
                                    float minDistance, float maxDistance) {
    if (!m_stream || m_voices.empty() || !clip || clip->GetFrameCount() == 0)
        return {};
    if (clip->GetSampleRate() != m_sampleRate) {
        SLEAK_WARN("AudioClip is {} Hz but the mixer runs at {} Hz",
                   clip->GetSampleRate(), m_sampleRate);
        return {};
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    Voice* slot = nullptr;
    for (Voice& v : m_voices)
        if (!v.inUse) {
            slot = &v;
            break;
        }
    if (!slot) {
        for (Voice& v : m_voices) {
            const bool better =
                !slot || (slot->loop && !v.loop) ||
                (slot->loop == v.loop && v.startOrder < slot->startOrder);
            if (better) slot = &v;
        }
        Release(*slot);
    }

    slot->clip = clip;
    slot->samples = clip->GetSamples();
    slot->frames = clip->GetFrameCount();
    slot->cursor = 0;
    slot->startOrder = ++m_playCounter;
    slot->volume = std::max(volume, 0.0f);
    slot->loop = loop;
    slot->spatial = position != nullptr;
    if (position) slot->position = *position;
    slot->minDistance = minDistance;
    slot->maxDistance = maxDistance;
    slot->fresh = true;
    slot->inUse = true;
    slot->playing = true;
    UpdateGains(*slot);

    return {static_cast<uint32_t>(slot - m_voices.data()), slot->generation};
}

void AudioSystem::Stop(AudioVoiceHandle voice) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (Voice* v = Resolve(voice)) Release(*v);
}

void AudioSystem::StopAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (Voice& v : m_voices)
        if (v.inUse) Release(v);
}

bool AudioSystem::IsPlaying(AudioVoiceHandle voice) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const Voice* v = Resolve(voice);
    return v && v->playing;
}

void AudioSystem::SetVolume(AudioVoiceHandle voice, float volume) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (Voice* v = Resolve(voice)) {
        v->volume = std::max(volume, 0.0f);
        UpdateGains(*v);
    }
}

void AudioSystem::SetVoicePosition(AudioVoiceHandle voice,
                                   const Math::Vector3D& position,
                                   float minDistance, float maxDistance) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (Voice* v = Resolve(voice)) {
        v->spatial = true;
        v->position = position;
        v->minDistance = minDistance;
        v->maxDistance = maxDistance;
        UpdateGains(*v);
    }
}

void AudioSystem::SetMasterVolume(float volume) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_masterVolume = std::max(volume, 0.0f);
}

float AudioSystem::GetMasterVolume() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_masterVolume;
}

void AudioSystem::SetListener(const Math::Vector3D& position,
                              const Math::Vector3D& forward,
                              const Math::Vector3D& up) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_followCamera = false;
    m_listenerPosition = position;
    m_listenerForward = forward;
    m_listenerUp = up;
    for (Voice& v : m_voices)
        if (v.inUse && v.spatial) UpdateGains(v);
}

void AudioSystem::SetListenerFollowsCamera(bool follow) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_followCamera = follow;
}

void AudioSystem::Update(const Camera* activeCamera) {
    if (!m_stream) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    const bool follow = m_followCamera && activeCamera;
    if (follow) {
        m_listenerPosition = activeCamera->GetPosition();
        m_listenerForward = activeCamera->GetDirection();
        m_listenerUp = activeCamera->GetUp();
    }

    for (Voice& v : m_voices) {
        if (!v.inUse) continue;
        if (!v.playing)
            Release(v);
        else if (follow && v.spatial)
            UpdateGains(v);
    }
}

void AudioSystem::Mix(float* out, uint32_t frames) {
    std::fill(out, out + size_t(frames) * 2, 0.0f);

    std::lock_guard<std::mutex> lock(m_mutex);
    const float master = m_masterVolume;
    const float invFrames = frames ? 1.0f / float(frames) : 0.0f;

    for (Voice& v : m_voices) {
        if (!v.inUse || !v.playing) continue;
        if (v.fresh) {
            v.left = v.targetLeft;
            v.right = v.targetRight;
            v.fresh = false;
        }

        // ramp toward the target gains across the block to avoid zipper noise
        const float stepL = (v.targetLeft - v.left) * invFrames;
        const float stepR = (v.targetRight - v.right) * invFrames;
        float gl = v.left;
        float gr = v.right;

        for (uint32_t i = 0; i < frames; ++i) {
            if (v.cursor >= v.frames) {
                if (!v.loop) {
                    v.playing = false;
                    break;
                }
                v.cursor = 0;
            }
            const float* s = v.samples + size_t(v.cursor) * 2;
            out[i * 2 + 0] += s[0] * gl * master;
            out[i * 2 + 1] += s[1] * gr * master;
            gl += stepL;
            gr += stepR;
            ++v.cursor;
        }
        if (v.playing && v.cursor >= v.frames && !v.loop) v.playing = false;
        v.left = v.targetLeft;
        v.right = v.targetRight;
    }

    for (size_t i = 0; i < size_t(frames) * 2; ++i)
        out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

void AudioSystem::StreamCallback(void* userdata, SDL_AudioStream* stream,
                                 int additionalAmount, int) {
    auto* self = static_cast<AudioSystem*>(userdata);
    uint32_t remaining =
        static_cast<uint32_t>(additionalAmount) / (sizeof(float) * 2);

    while (remaining > 0) {
        const uint32_t frames = std::min(remaining, kMixChunkFrames);
        self->Mix(self->m_mixBuffer.data(), frames);
        SDL_PutAudioStreamData(stream, self->m_mixBuffer.data(),
                               static_cast<int>(frames * sizeof(float) * 2));
        remaining -= frames;
    }
}

}  // namespace Sleak
