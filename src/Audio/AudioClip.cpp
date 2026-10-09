#include <SDL3/SDL.h>

#include <Audio/AudioClip.hpp>
#include <Audio/AudioSystem.hpp>
#include <Core/Logger.hpp>
#include <cstring>
#include <utility>

namespace Sleak {

AudioClip::AudioClip(std::vector<float> samples, uint32_t sampleRate)
    : m_samples(std::move(samples)), m_sampleRate(sampleRate) {
    m_samples.resize(m_samples.size() & ~size_t(1));
}

RefPtr<AudioClip> AudioClip::LoadWAV(const std::string& path) {
    SDL_AudioSpec srcSpec{};
    Uint8* srcData = nullptr;
    Uint32 srcLen = 0;
    if (!SDL_LoadWAV(path.c_str(), &srcSpec, &srcData, &srcLen)) {
        SLEAK_ERROR("Failed to load WAV '{}': {}", path, SDL_GetError());
        return RefPtr<AudioClip>();
    }

    const AudioSystem* audio = AudioSystem::GetInstance();
    const int rate = audio ? static_cast<int>(audio->GetSampleRate()) : 48000;
    const SDL_AudioSpec dstSpec{SDL_AUDIO_F32, 2, rate};

    Uint8* dstData = nullptr;
    int dstLen = 0;
    const bool converted =
        SDL_ConvertAudioSamples(&srcSpec, srcData, static_cast<int>(srcLen),
                                &dstSpec, &dstData, &dstLen);
    SDL_free(srcData);
    if (!converted) {
        SLEAK_ERROR("Failed to convert WAV '{}': {}", path, SDL_GetError());
        return RefPtr<AudioClip>();
    }

    std::vector<float> samples(static_cast<size_t>(dstLen) / sizeof(float));
    std::memcpy(samples.data(), dstData, samples.size() * sizeof(float));
    SDL_free(dstData);

    return RefPtr<AudioClip>(
        new AudioClip(std::move(samples), static_cast<uint32_t>(rate)));
}

}  // namespace Sleak
