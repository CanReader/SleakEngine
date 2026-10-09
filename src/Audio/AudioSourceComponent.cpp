#include <Core/GameObject.hpp>
#include <ECS/Components/AudioSourceComponent.hpp>
#include <ECS/Components/TransformComponent.hpp>
#include <utility>

namespace Sleak {

AudioSourceComponent::AudioSourceComponent(GameObject* owner,
                                           RefPtr<AudioClip> clip, float volume,
                                           bool loop, bool playOnEnable,
                                           bool spatial)
    : Component(owner),
      m_clip(std::move(clip)),
      m_volume(volume),
      m_loop(loop),
      m_playOnEnable(playOnEnable),
      m_spatial(spatial) {}

bool AudioSourceComponent::Initialize() {
    m_transform = GetOwner()->GetComponent<TransformComponent>();
    bIsInitialized = true;
    // GameObject::Initialize does not fire OnEnable, so start here as well
    if (GetOwner()->IsActive()) OnEnable();
    return true;
}

void AudioSourceComponent::Update(float) {
    if (m_spatial) PushPosition();
}

void AudioSourceComponent::OnEnable() {
    if (m_playOnEnable && !IsPlaying()) Play();
}

void AudioSourceComponent::OnDisable() { Stop(); }

void AudioSourceComponent::OnDestroy() { Stop(); }

void AudioSourceComponent::Play() {
    AudioSystem* audio = AudioSystem::GetInstance();
    if (!audio) return;
    audio->Stop(m_voice);
    if (m_spatial && m_transform)
        m_voice = audio->PlayAt(m_clip, m_transform->GetWorldPosition(),
                                m_volume, m_loop, m_minDistance, m_maxDistance);
    else
        m_voice = audio->Play(m_clip, m_volume, m_loop);
}

void AudioSourceComponent::Stop() {
    if (AudioSystem* audio = AudioSystem::GetInstance()) audio->Stop(m_voice);
    m_voice = {};
}

bool AudioSourceComponent::IsPlaying() const {
    const AudioSystem* audio = AudioSystem::GetInstance();
    return audio && audio->IsPlaying(m_voice);
}

void AudioSourceComponent::SetVolume(float volume) {
    m_volume = volume;
    if (AudioSystem* audio = AudioSystem::GetInstance())
        audio->SetVolume(m_voice, volume);
}

void AudioSourceComponent::SetDistances(float minDistance, float maxDistance) {
    m_minDistance = minDistance;
    m_maxDistance = maxDistance;
    if (m_spatial) PushPosition();
}

void AudioSourceComponent::PushPosition() {
    AudioSystem* audio = AudioSystem::GetInstance();
    if (!audio || !m_transform) return;
    audio->SetVoicePosition(m_voice, m_transform->GetWorldPosition(),
                            m_minDistance, m_maxDistance);
}

}  // namespace Sleak
