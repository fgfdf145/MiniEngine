#include "sound_event.h"

#include <algorithm>
#include <cmath>

namespace me
{

namespace
{
// FMOD's fader bottom: quieter than this is silence.
constexpr float kSilentDb = -80.0f;
constexpr float kLoudestDb = 24.0f;

float DbToGain(float db)
{
    return db <= kSilentDb ? 0.0f : std::pow(10.0f, std::min(db, kLoudestDb) / 20.0f);
}
}

SoundEventInstance::SoundEventInstance(AudioEngine& audio, const SoundEventDesc& desc)
    : m_audio(audio), m_desc(desc)
{
}

std::unique_ptr<SoundEventInstance> SoundEventInstance::Create(AudioEngine& audio, const SoundEventDesc& desc, const std::filesystem::path& directory, bool spatial,
                                                               std::string& error)
{
    std::unique_ptr<SoundEventInstance> instance(new SoundEventInstance(audio, desc));
    for (const SoundParameter& parameter : desc.parameters)
    {
        const float value = std::clamp(parameter.defaultValue, parameter.minimum, parameter.maximum);
        instance->m_parameters.push_back({value, value});
    }
    const bool loopingTimeline = desc.timelineLoops;
    for (const SoundInstrument& instrument : desc.instruments)
    {
        Voice voice;
        for (const SoundClip& clip : instrument.clips)
        {
            SoundDesc sound;
            sound.spatial = spatial;
            sound.looping = instrument.looping || (loopingTimeline && instrument.sheetParameter.empty());
            sound.volume = 0.0f;
            // A car is heard from far off; inside a few metres it is as loud as it gets.
            sound.minDistance = 3.0f;
            sound.maxDistance = 500.0f;
            // The listener has no velocity (it follows the camera, which chases the car), so a moving
            // car would sound sharp from its own speed.
            sound.dopplerFactor = 0.0f;
            const SoundId id = audio.CreateSound(directory / clip.file, sound, error);
            if (!id)
            {
                error = "'" + desc.name + "': " + error;
                return nullptr;
            }
            voice.clips.push_back(id);
        }
        instance->m_voices.push_back(std::move(voice));
    }
    return instance;
}

SoundEventInstance::~SoundEventInstance()
{
    for (Voice& voice : m_voices)
    {
        for (const SoundId id : voice.clips)
        {
            m_audio.DestroySound(id);
        }
    }
}

float SoundEventInstance::Value(std::string_view name) const
{
    for (size_t index = 0; index < m_desc.parameters.size(); ++index)
    {
        if (m_desc.parameters[index].name == name)
        {
            return m_parameters[index].value;
        }
    }
    return 0.0f;
}

float SoundEventInstance::Parameter(std::string_view name) const
{
    return Value(name);
}

void SoundEventInstance::SetParameter(std::string_view name, float value)
{
    for (size_t index = 0; index < m_desc.parameters.size(); ++index)
    {
        const SoundParameter& parameter = m_desc.parameters[index];
        if (parameter.name != name)
        {
            continue;
        }
        ParameterState& state = m_parameters[index];
        state.target = std::clamp(value, parameter.minimum, parameter.maximum);
        if (parameter.seekSpeed <= 0.0f)
        {
            state.value = state.target;
        }
        return;
    }
}

void SoundEventInstance::SetVolume(float volume)
{
    m_volume = std::max(volume, 0.0f);
}

void SoundEventInstance::SetPitch(float pitch)
{
    m_pitch = std::max(pitch, 0.01f);
}

void SoundEventInstance::SetPosition(const glm::vec3& position, const glm::vec3& velocity)
{
    for (const Voice& voice : m_voices)
    {
        for (const SoundId id : voice.clips)
        {
            m_audio.SetPosition(id, position);
            m_audio.SetVelocity(id, velocity);
        }
    }
}

bool SoundEventInstance::Inside(const SoundInstrument& instrument) const
{
    const SoundParameter* parameter = m_desc.FindParameter(instrument.sheetParameter);
    if (parameter == nullptr)
    {
        return false;
    }
    const float value = Value(instrument.sheetParameter);
    const float end = instrument.start + instrument.length;
    // Regions are half open, except that one reaching the parameter's top takes the top in.
    return value >= instrument.start && (value < end || (end >= parameter->maximum && value <= end));
}

void SoundEventInstance::StartVoice(size_t index)
{
    Voice& voice = m_voices[index];
    if (voice.clips.empty())
    {
        return;
    }
    m_audio.Stop(voice.clips[voice.current]);
    if (voice.clips.size() > 1)
    {
        voice.current = std::uniform_int_distribution<size_t>(0, voice.clips.size() - 1)(m_random);
    }
    voice.active = true;
    m_audio.Play(voice.clips[voice.current]);
}

void SoundEventInstance::StopVoice(size_t index)
{
    Voice& voice = m_voices[index];
    voice.active = false;
    for (const SoundId id : voice.clips)
    {
        m_audio.Stop(id);
    }
}

void SoundEventInstance::Start()
{
    Stop();
    m_playing = true;
    m_elapsed = 0.0f;
    for (size_t index = 0; index < m_voices.size(); ++index)
    {
        m_voices[index].inside = false;
        m_voices[index].triggered = false;
    }
    Update(0.0f);
}

void SoundEventInstance::Stop()
{
    m_playing = false;
    for (size_t index = 0; index < m_voices.size(); ++index)
    {
        StopVoice(index);
        m_voices[index].state = VoiceState{};
    }
}

void SoundEventInstance::AddAutomations(const std::vector<SoundAutomation>& automations, float& volumeDb, float& pitchSemitones, float& fade) const
{
    for (const SoundAutomation& automation : automations)
    {
        const float value = EvaluateSoundMapping(automation.mapping, EvaluateSoundCurve(automation.curve, Value(automation.parameter)));
        switch (automation.property)
        {
        case SoundProperty::Volume:
            volumeDb += value;
            break;
        case SoundProperty::Pitch:
            pitchSemitones += value;
            break;
        case SoundProperty::Fade:
            fade *= std::clamp(value, 0.0f, 1.0f);
            break;
        }
    }
}

float SoundEventInstance::InstrumentVolumeDb(size_t index) const
{
    const SoundInstrument& instrument = m_desc.instruments[index];
    float volumeDb = instrument.volumeDb;
    float pitch = 0.0f;
    float fade = 1.0f;
    AddAutomations(instrument.automations, volumeDb, pitch, fade);
    for (int bus = instrument.bus, depth = 0; bus >= 0 && depth < 32; bus = m_desc.buses[static_cast<size_t>(bus)].parent, ++depth)
    {
        volumeDb += m_desc.buses[static_cast<size_t>(bus)].volumeDb;
        AddAutomations(m_desc.buses[static_cast<size_t>(bus)].automations, volumeDb, pitch, fade);
    }
    return volumeDb;
}

float SoundEventInstance::InstrumentFade(size_t index) const
{
    const SoundInstrument& instrument = m_desc.instruments[index];
    float volumeDb = 0.0f;
    float pitch = 0.0f;
    float fade = 1.0f;
    AddAutomations(instrument.automations, volumeDb, pitch, fade);
    for (int bus = instrument.bus, depth = 0; bus >= 0 && depth < 32; bus = m_desc.buses[static_cast<size_t>(bus)].parent, ++depth)
    {
        AddAutomations(m_desc.buses[static_cast<size_t>(bus)].automations, volumeDb, pitch, fade);
    }
    return fade;
}

float SoundEventInstance::InstrumentPitch(size_t index) const
{
    const SoundInstrument& instrument = m_desc.instruments[index];
    float volumeDb = 0.0f;
    float semitones = instrument.pitchSemitones;
    float fade = 1.0f;
    AddAutomations(instrument.automations, volumeDb, semitones, fade);
    for (int bus = instrument.bus, depth = 0; bus >= 0 && depth < 32; bus = m_desc.buses[static_cast<size_t>(bus)].parent, ++depth)
    {
        AddAutomations(m_desc.buses[static_cast<size_t>(bus)].automations, volumeDb, semitones, fade);
    }
    float ratio = std::exp2(semitones / 12.0f);
    if (!instrument.autopitchParameter.empty())
    {
        const SoundParameter* parameter = m_desc.FindParameter(instrument.autopitchParameter);
        const float minimum = parameter != nullptr ? parameter->minimum : 0.0f;
        const float span = instrument.autopitchReference - minimum;
        if (span > 0.0f)
        {
            const float t = (Value(instrument.autopitchParameter) - minimum) / span;
            ratio *= instrument.autopitchMinimum + (1.0f - instrument.autopitchMinimum) * t;
        }
    }
    return std::max(ratio, 0.01f);
}

SoundEventInstance::VoiceState SoundEventInstance::VoiceStateOf(size_t instrument) const
{
    return instrument < m_voices.size() ? m_voices[instrument].state : VoiceState{};
}

void SoundEventInstance::Update(float deltaSeconds)
{
    for (size_t index = 0; index < m_parameters.size(); ++index)
    {
        ParameterState& state = m_parameters[index];
        const float speed = m_desc.parameters[index].seekSpeed;
        if (speed > 0.0f && state.value != state.target)
        {
            const float step = speed * std::max(deltaSeconds, 0.0f);
            state.value = state.value < state.target ? std::min(state.value + step, state.target) : std::max(state.value - step, state.target);
        }
    }
    if (!m_playing)
    {
        return;
    }
    m_elapsed += std::max(deltaSeconds, 0.0f);

    for (size_t index = 0; index < m_voices.size(); ++index)
    {
        const SoundInstrument& instrument = m_desc.instruments[index];
        Voice& voice = m_voices[index];
        if (instrument.sheetParameter.empty())
        {
            // On the timeline: once, when the playhead reaches it.
            if (!voice.triggered && m_elapsed >= instrument.start)
            {
                voice.triggered = true;
                StartVoice(index);
            }
        }
        else
        {
            const bool inside = Inside(instrument);
            if (inside && !voice.inside)
            {
                StartVoice(index);
            }
            else if (!inside && voice.inside && instrument.looping)
            {
                StopVoice(index);
            }
            voice.inside = inside;
        }

        if (voice.active && !voice.clips.empty() && !m_audio.IsPlaying(voice.clips[voice.current]))
        {
            voice.active = false;
        }
        const SoundId sound = voice.clips.empty() ? SoundId{} : voice.clips[voice.current];
        const float clipDb = instrument.clips.empty() ? 0.0f : instrument.clips[voice.current].volumeDb;
        voice.state.playing = voice.active;
        voice.state.gain = voice.active ? DbToGain(InstrumentVolumeDb(index) + clipDb) * InstrumentFade(index) * m_volume : 0.0f;
        voice.state.pitch = InstrumentPitch(index) * m_pitch;
        if (voice.active)
        {
            m_audio.SetVolume(sound, voice.state.gain);
            m_audio.SetPitch(sound, voice.state.pitch);
        }
    }
}

}
