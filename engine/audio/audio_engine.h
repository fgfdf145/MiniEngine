#pragma once

#include "audio_file.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace me
{

// Where the mix goes.
enum class AudioOutput
{
    // The system's default playback device; the mix runs on miniaudio's audio thread.
    Device,
    // No device: nothing plays and time stands still until Render pulls frames (tests, scripted runs).
    None,
};

struct AudioEngineOptions
{
    AudioOutput output = AudioOutput::Device;
    // 0 takes the device's own rate (48 kHz without a device).
    uint32_t sampleRate = 0;
    // 0 takes the device's own channel count (stereo without a device).
    uint32_t channels = 0;
};

// A sound made by AudioEngine::CreateSound; 0 is no sound. Ids are not reused.
struct SoundId
{
    uint32_t value = 0;

    explicit operator bool() const
    {
        return value != 0;
    }
    bool operator==(const SoundId&) const = default;
};

struct SoundDesc
{
    // Positioned in the world and heard from the listener; otherwise played straight to the speakers.
    bool spatial = false;
    bool looping = false;
    // Decoded while it plays rather than all at once on creation: for music and long ambiences.
    // Decoded sounds share their samples: a file loaded twice is decoded once.
    bool stream = false;
    float volume = 1.0f;
    float pitch = 1.0f;
    // Spatial sounds only, in metres. Inverse-distance attenuation: full volume within minDistance,
    // no quieter past maxDistance.
    glm::vec3 position{0.0f};
    float minDistance = 1.0f;
    float maxDistance = 1000.0f;
    float rolloff = 1.0f;
    // Scales the pitch shift of a moving source or listener; 0 turns it off.
    float dopplerFactor = 1.0f;
};

// The engine's sound: a miniaudio engine (a mixer with one listener) and the sounds made from it.
// Calls come from one thread, the main one; the mix itself runs on the audio thread.
class AudioEngine final
{
  public:
    // Null, with the reason in error, when miniaudio cannot start (no playback device, for one).
    static std::unique_ptr<AudioEngine> Create(const AudioEngineOptions& options, std::string& error);
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    AudioOutput Output() const;
    uint32_t SampleRate() const;
    uint32_t Channels() const;
    // The playback device's name, or "None".
    std::string DeviceName() const;

    // Linear gain over every sound, clamped to [0, 4]. 1 leaves the mix as it is.
    void SetMasterVolume(float volume);
    float MasterVolume() const;

    // The listener, where spatial sounds are heard from. forward and up need not be normalized; the
    // velocity (metres a second) only shifts the pitch (Doppler).
    void SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up, const glm::vec3& velocity = glm::vec3(0.0f));

    // A sound from a .wav, .mp3, .flac or .ogg file, stopped; nothing, with the reason in error,
    // when it cannot be read.
    SoundId CreateSound(const std::filesystem::path& path, const SoundDesc& desc, std::string& error);
    void DestroySound(SoundId sound);
    // Plays from where it was stopped, or from the start once it reached its end.
    bool Play(SoundId sound);
    // Stops and rewinds.
    void Stop(SoundId sound);
    void Pause(SoundId sound);
    bool IsPlaying(SoundId sound) const;
    void SetVolume(SoundId sound, float volume);
    void SetPitch(SoundId sound, float pitch);
    void SetLooping(SoundId sound, bool looping);
    void SetPosition(SoundId sound, const glm::vec3& position);
    void SetVelocity(SoundId sound, const glm::vec3& velocity);
    // 0 when the length is not known: a streamed Ogg Vorbis file, for one.
    float LengthSeconds(SoundId sound) const;
    float CursorSeconds(SoundId sound) const;

    // Plays a file once, not spatialized, and forgets it; Update frees it when it has finished.
    bool PlayOneShot(const std::filesystem::path& path, float volume, std::string& error);

    // The asset browser's preview: one file at a time, streamed. Starting a file stops the one before.
    bool StartPreview(const std::filesystem::path& path, std::string& error);
    void StopPreview();
    // The file the preview is playing, or empty.
    std::filesystem::path PreviewPath() const;

    // Once a frame: frees the one-shots and the preview that have finished.
    void Update();

    // AudioOutput::None only: mixes the next frameCount frames into out (interleaved floats, Channels()
    // per frame) and returns how many the mix produced; the rest of out is silence (with no sound
    // playing the mix produces none).
    uint64_t Render(float* out, uint64_t frameCount);

  private:
    struct Impl;
    explicit AudioEngine(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

}
