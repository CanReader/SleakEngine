#ifndef _AUDIOCLIP_HPP_
#define _AUDIOCLIP_HPP_

#include <Core/OSDef.hpp>
#include <Memory/RefPtr.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace Sleak {

/// Fully decoded sound, stored as interleaved stereo float samples at the
/// mixer's sample rate so playback never converts on the audio thread.
///
/// Load one with LoadWAV() and hand the RefPtr to AudioSystem::Play() or an
/// AudioSourceComponent. Voices keep their own reference, so dropping yours
/// while the sound plays is fine.
/// @ingroup audio
class ENGINE_API AudioClip {
   public:
    /// Loads a WAV file and converts it to the mixer format. Returns an
    /// empty RefPtr on failure.
    static RefPtr<AudioClip> LoadWAV(const std::string& path);

    /// Wraps already interleaved stereo float samples at sampleRate.
    AudioClip(std::vector<float> samples, uint32_t sampleRate);

    const float* GetSamples() const { return m_samples.data(); }
    /// Number of stereo frames (samples per channel).
    uint32_t GetFrameCount() const {
        return static_cast<uint32_t>(m_samples.size() / 2);
    }
    uint32_t GetSampleRate() const { return m_sampleRate; }
    float GetDuration() const {
        return m_sampleRate ? float(GetFrameCount()) / float(m_sampleRate)
                            : 0.0f;
    }

   private:
    std::vector<float> m_samples;
    uint32_t m_sampleRate = 0;
};

}  // namespace Sleak

#endif  // _AUDIOCLIP_HPP_
