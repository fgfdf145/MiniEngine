#include "audio_engine.h"

#include <engine/core/log/log.h>

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <unordered_map>
#include <utility>
#include <vector>

namespace me
{

namespace
{
// Sounds stay where miniaudio initialized them: its node graph holds their addresses.
using SoundPtr = std::unique_ptr<ma_sound>;

std::string ResultText(ma_result result)
{
    return ma_result_description(result);
}

ma_result InitSoundFromFile(ma_engine& engine, const std::filesystem::path& path, ma_uint32 flags, ma_sound& sound)
{
#ifdef _WIN32
    // The wide path: file names that are not ASCII open whatever the process code page.
    return ma_sound_init_from_file_w(&engine, path.c_str(), flags, nullptr, nullptr, &sound);
#else
    return ma_sound_init_from_file(&engine, path.c_str(), flags, nullptr, nullptr, &sound);
#endif
}

// A sound from a file, or null with the reason in error.
SoundPtr LoadSound(ma_engine& engine, const std::filesystem::path& path, ma_uint32 flags, std::string& error)
{
    std::error_code existsError;
    if (!std::filesystem::is_regular_file(path, existsError))
    {
        error = "'" + path.string() + "' is not a file";
        return nullptr;
    }
    SoundPtr sound = std::make_unique<ma_sound>();
    const ma_result result = InitSoundFromFile(engine, path, flags, *sound);
    if (result != MA_SUCCESS)
    {
        error = "Cannot play '" + path.string() + "': " + ResultText(result);
        return nullptr;
    }
    return sound;
}

void FreeSound(SoundPtr& sound)
{
    if (sound)
    {
        ma_sound_uninit(sound.get());
        sound.reset();
    }
}
}

struct AudioEngine::Impl
{
    ma_engine engine{};
    bool engineInitialized = false;
    AudioOutput output = AudioOutput::Device;
    std::unordered_map<uint32_t, SoundPtr> sounds;
    uint32_t nextSoundId = 1;
    std::vector<SoundPtr> oneShots;
    SoundPtr preview;
    std::filesystem::path previewPath;

    ~Impl()
    {
        for (auto& [id, sound] : sounds)
        {
            FreeSound(sound);
        }
        for (SoundPtr& sound : oneShots)
        {
            FreeSound(sound);
        }
        FreeSound(preview);
        if (engineInitialized)
        {
            ma_engine_uninit(&engine);
        }
    }

    ma_sound* Find(SoundId id) const
    {
        const auto found = sounds.find(id.value);
        return found != sounds.end() ? found->second.get() : nullptr;
    }
};

std::unique_ptr<AudioEngine> AudioEngine::Create(const AudioEngineOptions& options, std::string& error)
{
    auto impl = std::make_unique<Impl>();
    impl->output = options.output;

    ma_engine_config config = ma_engine_config_init();
    config.listenerCount = 1;
    config.noDevice = options.output == AudioOutput::None ? MA_TRUE : MA_FALSE;
    // Without a device there is nothing to take the format from.
    config.sampleRate = options.sampleRate != 0 ? options.sampleRate : (config.noDevice ? 48000u : 0u);
    config.channels = options.channels != 0 ? options.channels : (config.noDevice ? 2u : 0u);
    const ma_result result = ma_engine_init(&config, &impl->engine);
    if (result != MA_SUCCESS)
    {
        error = "miniaudio could not start: " + ResultText(result);
        return nullptr;
    }
    impl->engineInitialized = true;

    std::unique_ptr<AudioEngine> audio(new AudioEngine(std::move(impl)));
    LOG_INFO(
        "Audio: miniaudio {} on '{}', {} Hz, {} channels",
        MA_VERSION_STRING,
        audio->DeviceName(),
        audio->SampleRate(),
        audio->Channels());
    return audio;
}

AudioEngine::AudioEngine(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

AudioEngine::~AudioEngine() = default;

AudioOutput AudioEngine::Output() const
{
    return m_impl->output;
}

uint32_t AudioEngine::SampleRate() const
{
    return ma_engine_get_sample_rate(&m_impl->engine);
}

uint32_t AudioEngine::Channels() const
{
    return ma_engine_get_channels(&m_impl->engine);
}

std::string AudioEngine::DeviceName() const
{
    ma_device* device = ma_engine_get_device(&m_impl->engine);
    if (device == nullptr)
    {
        return "None";
    }
    std::array<char, MA_MAX_DEVICE_NAME_LENGTH + 1> name{};
    size_t length = 0;
    if (ma_device_get_name(device, ma_device_type_playback, name.data(), name.size(), &length) != MA_SUCCESS)
    {
        return "Default device";
    }
    return std::string(name.data(), length);
}

void AudioEngine::SetMasterVolume(float volume)
{
    ma_engine_set_volume(&m_impl->engine, std::clamp(volume, 0.0f, 4.0f));
}

float AudioEngine::MasterVolume() const
{
    return ma_engine_get_volume(&m_impl->engine);
}

void AudioEngine::SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up, const glm::vec3& velocity)
{
    ma_engine* engine = &m_impl->engine;
    ma_engine_listener_set_position(engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(engine, 0, up.x, up.y, up.z);
    ma_engine_listener_set_velocity(engine, 0, velocity.x, velocity.y, velocity.z);
}

SoundId AudioEngine::CreateSound(const std::filesystem::path& path, const SoundDesc& desc, std::string& error)
{
    ma_uint32 flags = desc.stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
    if (!desc.spatial)
    {
        flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
    }
    if (desc.looping)
    {
        flags |= MA_SOUND_FLAG_LOOPING;
    }
    SoundPtr sound = LoadSound(m_impl->engine, path, flags, error);
    if (!sound)
    {
        return {};
    }

    ma_sound_set_volume(sound.get(), std::max(desc.volume, 0.0f));
    ma_sound_set_pitch(sound.get(), std::max(desc.pitch, 0.0f));
    if (desc.spatial)
    {
        ma_sound_set_position(sound.get(), desc.position.x, desc.position.y, desc.position.z);
        ma_sound_set_attenuation_model(sound.get(), ma_attenuation_model_inverse);
        ma_sound_set_min_distance(sound.get(), std::max(desc.minDistance, 0.0f));
        ma_sound_set_max_distance(sound.get(), std::max(desc.maxDistance, desc.minDistance));
        ma_sound_set_rolloff(sound.get(), std::max(desc.rolloff, 0.0f));
        ma_sound_set_doppler_factor(sound.get(), std::max(desc.dopplerFactor, 0.0f));
    }

    const SoundId id{m_impl->nextSoundId++};
    m_impl->sounds.emplace(id.value, std::move(sound));
    return id;
}

void AudioEngine::DestroySound(SoundId sound)
{
    const auto found = m_impl->sounds.find(sound.value);
    if (found != m_impl->sounds.end())
    {
        FreeSound(found->second);
        m_impl->sounds.erase(found);
    }
}

bool AudioEngine::Play(SoundId sound)
{
    ma_sound* const found = m_impl->Find(sound);
    if (found == nullptr)
    {
        return false;
    }
    // A sound at its end may not have been stopped yet, and ma_sound_start leaves a playing sound
    // alone; stopped, it starts again from the beginning.
    if (ma_sound_at_end(found))
    {
        ma_sound_stop(found);
    }
    return ma_sound_start(found) == MA_SUCCESS;
}

void AudioEngine::Stop(SoundId sound)
{
    if (ma_sound* const found = m_impl->Find(sound))
    {
        ma_sound_stop(found);
        ma_sound_seek_to_pcm_frame(found, 0);
    }
}

void AudioEngine::Pause(SoundId sound)
{
    if (ma_sound* const found = m_impl->Find(sound))
    {
        ma_sound_stop(found);
    }
}

bool AudioEngine::IsPlaying(SoundId sound) const
{
    const ma_sound* const found = m_impl->Find(sound);
    // miniaudio marks a sound at its end at once but stops it a mixing step later.
    return found != nullptr && ma_sound_is_playing(found) && !ma_sound_at_end(found);
}

void AudioEngine::SetVolume(SoundId sound, float volume)
{
    if (ma_sound* const found = m_impl->Find(sound))
    {
        ma_sound_set_volume(found, std::max(volume, 0.0f));
    }
}

void AudioEngine::SetPitch(SoundId sound, float pitch)
{
    if (ma_sound* const found = m_impl->Find(sound))
    {
        ma_sound_set_pitch(found, std::max(pitch, 0.0f));
    }
}

void AudioEngine::SetLooping(SoundId sound, bool looping)
{
    if (ma_sound* const found = m_impl->Find(sound))
    {
        ma_sound_set_looping(found, looping ? MA_TRUE : MA_FALSE);
    }
}

void AudioEngine::SetPosition(SoundId sound, const glm::vec3& position)
{
    if (ma_sound* const found = m_impl->Find(sound))
    {
        ma_sound_set_position(found, position.x, position.y, position.z);
    }
}

void AudioEngine::SetVelocity(SoundId sound, const glm::vec3& velocity)
{
    if (ma_sound* const found = m_impl->Find(sound))
    {
        ma_sound_set_velocity(found, velocity.x, velocity.y, velocity.z);
    }
}

float AudioEngine::LengthSeconds(SoundId sound) const
{
    float length = 0.0f;
    if (ma_sound* const found = m_impl->Find(sound))
    {
        ma_sound_get_length_in_seconds(found, &length);
    }
    return length;
}

float AudioEngine::CursorSeconds(SoundId sound) const
{
    float cursor = 0.0f;
    if (ma_sound* const found = m_impl->Find(sound))
    {
        ma_sound_get_cursor_in_seconds(found, &cursor);
    }
    return cursor;
}

bool AudioEngine::PlayOneShot(const std::filesystem::path& path, float volume, std::string& error)
{
    SoundPtr sound = LoadSound(m_impl->engine, path, MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION, error);
    if (!sound)
    {
        return false;
    }
    ma_sound_set_volume(sound.get(), std::max(volume, 0.0f));
    const ma_result result = ma_sound_start(sound.get());
    if (result != MA_SUCCESS)
    {
        error = "Cannot play '" + path.string() + "': " + ResultText(result);
        FreeSound(sound);
        return false;
    }
    m_impl->oneShots.push_back(std::move(sound));
    return true;
}

bool AudioEngine::StartPreview(const std::filesystem::path& path, std::string& error)
{
    StopPreview();
    SoundPtr sound = LoadSound(m_impl->engine, path, MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION, error);
    if (!sound)
    {
        return false;
    }
    const ma_result result = ma_sound_start(sound.get());
    if (result != MA_SUCCESS)
    {
        error = "Cannot play '" + path.string() + "': " + ResultText(result);
        FreeSound(sound);
        return false;
    }
    m_impl->preview = std::move(sound);
    m_impl->previewPath = path;
    return true;
}

void AudioEngine::StopPreview()
{
    FreeSound(m_impl->preview);
    m_impl->previewPath.clear();
}

std::filesystem::path AudioEngine::PreviewPath() const
{
    return m_impl->previewPath;
}

void AudioEngine::Update()
{
    std::vector<SoundPtr>& oneShots = m_impl->oneShots;
    for (SoundPtr& sound : oneShots)
    {
        if (ma_sound_at_end(sound.get()))
        {
            FreeSound(sound);
        }
    }
    std::erase(oneShots, nullptr);
    if (m_impl->preview && ma_sound_at_end(m_impl->preview.get()))
    {
        StopPreview();
    }
}

uint64_t AudioEngine::Render(float* out, uint64_t frameCount)
{
    if (m_impl->output != AudioOutput::None)
    {
        return 0;
    }
    ma_uint64 framesRead = 0;
    ma_engine_read_pcm_frames(&m_impl->engine, out, frameCount, &framesRead);
    // An empty mix, or a stream still waiting for its pages, ends the read early: the rest is silence.
    std::fill(out + framesRead * Channels(), out + frameCount * Channels(), 0.0f);
    return framesRead;
}
}
