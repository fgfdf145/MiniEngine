#pragma once

#include "audio_engine.h"
#include "sound_bank.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace me
{

// One event of a SoundBank playing on an AudioEngine, the way FMOD Studio plays it: instruments on a
// parameter sheet play while the parameter is inside their region (looping ones for as long as it stays,
// one-shots once each time it enters), timeline instruments play from the event's start, and every
// voice's volume and pitch follow its instrument's and buses' automations of the parameters. Calls come
// from the main thread, as AudioEngine's do; Update applies what changed.
class SoundEventInstance final
{
  public:
    // Null, with the reason in error, when a sound file cannot be read. Files are relative to `directory`.
    // A spatial event is heard from where SetPosition puts it.
    static std::unique_ptr<SoundEventInstance> Create(AudioEngine& audio, const SoundEventDesc& desc, const std::filesystem::path& directory, bool spatial,
                                                      std::string& error);
    ~SoundEventInstance();

    SoundEventInstance(const SoundEventInstance&) = delete;
    SoundEventInstance& operator=(const SoundEventInstance&) = delete;

    const SoundEventDesc& Desc() const
    {
        return m_desc;
    }

    // Starts the timeline from the beginning and the sheet instruments at the parameters' values; a
    // started event starts again.
    void Start();
    void Stop();
    bool IsPlaying() const
    {
        return m_playing;
    }

    // The value the parameter moves to (at once, or at its seek speed), clamped to its range. A parameter
    // the event does not have is ignored.
    void SetParameter(std::string_view name, float value);
    // Where the parameter is now; 0 for one the event does not have.
    float Parameter(std::string_view name) const;

    // Linear gain and pitch ratio over everything the event plays (AC turns the skids up and down so).
    void SetVolume(float volume);
    void SetPitch(float pitch);
    void SetPosition(const glm::vec3& position, const glm::vec3& velocity = glm::vec3(0.0f));

    // Moves the parameters on by deltaSeconds and sets every voice's volume and pitch.
    void Update(float deltaSeconds);

    // What Update last gave an instrument: its voice playing, its linear gain and its pitch ratio.
    struct VoiceState
    {
        bool playing = false;
        float gain = 0.0f;
        float pitch = 1.0f;
    };
    VoiceState VoiceStateOf(size_t instrument) const;

    // The instrument's volume (dB, without the event's own) and pitch (ratio) at the parameters' values
    // now, whether it plays or not.
    float InstrumentVolumeDb(size_t instrument) const;
    float InstrumentFade(size_t instrument) const;
    float InstrumentPitch(size_t instrument) const;

  private:
    struct ParameterState
    {
        float value = 0.0f;
        float target = 0.0f;
    };
    struct Voice
    {
        std::vector<SoundId> clips;
        size_t current = 0;
        bool inside = false;
        bool triggered = false;
        bool active = false;
        VoiceState state;
    };

    SoundEventInstance(AudioEngine& audio, const SoundEventDesc& desc);
    float Value(std::string_view name) const;
    bool Inside(const SoundInstrument& instrument) const;
    void StartVoice(size_t index);
    void StopVoice(size_t index);
    void AddAutomations(const std::vector<SoundAutomation>& automations, float& volumeDb, float& pitchSemitones, float& fade) const;

    AudioEngine& m_audio;
    SoundEventDesc m_desc;
    std::vector<ParameterState> m_parameters;
    std::vector<Voice> m_voices;
    bool m_playing = false;
    float m_elapsed = 0.0f;
    float m_volume = 1.0f;
    float m_pitch = 1.0f;
    std::minstd_rand m_random{0x5eed};
};

}
