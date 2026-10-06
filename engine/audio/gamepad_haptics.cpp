#include "gamepad_haptics.h"

#include <engine/core/log/log.h>

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <optional>

namespace me
{

namespace
{
// The actuators are the third and fourth of the pad's four channels.
constexpr uint32_t kFirstActuatorChannel = 2;
constexpr uint32_t kRequiredChannels = kFirstActuatorChannel + kHapticsSides;
// Short periods, so a bump is felt within a frame or two of the wheel meeting it.
constexpr uint32_t kPeriodMilliseconds = 10;
}

bool IsDualSenseAudioDeviceName(const std::string& name)
{
    // Windows calls it "Speakers (DualSense Wireless Controller)"; older drivers drop the "DualSense".
    return name.find("DualSense") != std::string::npos || name.find("Wireless Controller") != std::string::npos;
}

struct GamepadHaptics::Impl
{
    ma_context context{};
    bool contextInitialized = false;
    ma_device device{};
    bool deviceInitialized = false;
    std::string name;
    std::optional<HapticsSynth> synth;
    std::atomic<bool> lost = false;

    // Handed from the caller's thread to the audio thread, which takes them when the lock is free.
    std::mutex pendingMutex;
    HapticsVoices pendingVoices;
    std::array<float, kHapticsSides> pendingKicks{};

    ~Impl()
    {
        if (deviceInitialized)
        {
            ma_device_uninit(&device);
        }
        if (contextInitialized)
        {
            ma_context_uninit(&context);
        }
    }

    static void DataCallback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frameCount)
    {
        Impl& impl = *static_cast<Impl*>(device->pUserData);
        const uint32_t channels = device->playback.channels;
        float* const samples = static_cast<float*>(output);
        std::fill(samples, samples + static_cast<size_t>(frameCount) * channels, 0.0f);

        // The audio thread never waits: when the caller holds the lock, the last levels play on.
        if (std::unique_lock lock(impl.pendingMutex, std::try_to_lock); lock.owns_lock())
        {
            impl.synth->SetVoices(impl.pendingVoices);
            for (size_t side = 0; side < kHapticsSides; ++side)
            {
                if (impl.pendingKicks[side] > 0.0f)
                {
                    impl.synth->Kick(side, impl.pendingKicks[side]);
                    impl.pendingKicks[side] = 0.0f;
                }
            }
        }
        impl.synth->Render(samples + kFirstActuatorChannel, frameCount, channels);
    }

    static void NotificationCallback(const ma_device_notification* notification)
    {
        if (notification->type == ma_device_notification_type_stopped)
        {
            static_cast<Impl*>(notification->pDevice->pUserData)->lost = true;
        }
    }
};

std::unique_ptr<GamepadHaptics> GamepadHaptics::Open(std::string& error)
{
    auto impl = std::make_unique<Impl>();
    ma_result result = ma_context_init(nullptr, 0, nullptr, &impl->context);
    if (result != MA_SUCCESS)
    {
        error = std::string("miniaudio could not start: ") + ma_result_description(result);
        return nullptr;
    }
    impl->contextInitialized = true;

    ma_device_info* playbackDevices = nullptr;
    ma_uint32 playbackDeviceCount = 0;
    result = ma_context_get_devices(&impl->context, &playbackDevices, &playbackDeviceCount, nullptr, nullptr);
    if (result != MA_SUCCESS)
    {
        error = std::string("Cannot list the playback devices: ") + ma_result_description(result);
        return nullptr;
    }
    const ma_device_info* found = nullptr;
    for (ma_uint32 index = 0; index < playbackDeviceCount; ++index)
    {
        if (IsDualSenseAudioDeviceName(playbackDevices[index].name))
        {
            found = &playbackDevices[index];
            break;
        }
    }
    if (found == nullptr)
    {
        error = "No DualSense playback device (the pad needs a USB cable for its haptics)";
        return nullptr;
    }
    impl->name = found->name;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.pDeviceID = &found->id;
    config.playback.format = ma_format_f32;
    // The device's own channel count and rate: the actuators are its third and fourth channels as they are.
    config.playback.channels = 0;
    config.sampleRate = 0;
    config.periodSizeInMilliseconds = kPeriodMilliseconds;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.dataCallback = &Impl::DataCallback;
    config.notificationCallback = &Impl::NotificationCallback;
    config.pUserData = impl.get();
    result = ma_device_init(&impl->context, &config, &impl->device);
    if (result != MA_SUCCESS)
    {
        error = "Cannot open '" + impl->name + "': " + ma_result_description(result);
        return nullptr;
    }
    impl->deviceInitialized = true;

    if (impl->device.playback.channels < kRequiredChannels)
    {
        error = "'" + impl->name + "' has " + std::to_string(impl->device.playback.channels) + " channels; its haptics need " +
                std::to_string(kRequiredChannels);
        return nullptr;
    }
    impl->synth.emplace(impl->device.sampleRate);

    result = ma_device_start(&impl->device);
    if (result != MA_SUCCESS)
    {
        error = "Cannot start '" + impl->name + "': " + ma_result_description(result);
        return nullptr;
    }

    std::unique_ptr<GamepadHaptics> haptics(new GamepadHaptics(std::move(impl)));
    LOG_INFO("Gamepad haptics: '{}', {} Hz, {} channels (actuators on 3 and 4)", haptics->DeviceName(), haptics->SampleRate(),
             haptics->Channels());
    return haptics;
}

GamepadHaptics::GamepadHaptics(std::unique_ptr<Impl> impl)
    : m_impl(std::move(impl))
{
}

GamepadHaptics::~GamepadHaptics() = default;

void GamepadHaptics::SetVoices(const HapticsVoices& voices)
{
    std::lock_guard lock(m_impl->pendingMutex);
    m_impl->pendingVoices = voices;
}

void GamepadHaptics::Kick(size_t side, float strength)
{
    if (side >= kHapticsSides)
    {
        return;
    }
    std::lock_guard lock(m_impl->pendingMutex);
    m_impl->pendingKicks[side] = std::max(m_impl->pendingKicks[side], strength);
}

bool GamepadHaptics::IsRunning() const
{
    return !m_impl->lost && ma_device_get_state(&m_impl->device) == ma_device_state_started;
}

std::string GamepadHaptics::DeviceName() const
{
    return m_impl->name;
}

uint32_t GamepadHaptics::SampleRate() const
{
    return m_impl->device.sampleRate;
}

uint32_t GamepadHaptics::Channels() const
{
    return m_impl->device.playback.channels;
}
}
