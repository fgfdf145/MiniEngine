#pragma once

#include "haptics_synth.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace me
{

// A DualSense's haptic actuators, played as sound. On USB the pad is also a sound card, its playback
// device named "... Wireless Controller" with four channels: the first two are its speaker and headset,
// the third and fourth drive the left and right actuators. The waveforms (HapticsSynth) go to those two,
// and the speaker stays silent. Over Bluetooth there is no such device.
//
// The actuators only follow the sound while the pad's effects report leaves its rumble emulation off
// (GamepadFeedback::audioHaptics). Calls come from one thread; the waveforms are made on miniaudio's.
class GamepadHaptics final
{
  public:
    // The first DualSense playback device found, playing silence; null, with the reason in error, when
    // there is none or it cannot be opened.
    static std::unique_ptr<GamepadHaptics> Open(std::string& error);
    ~GamepadHaptics();

    GamepadHaptics(const GamepadHaptics&) = delete;
    GamepadHaptics& operator=(const GamepadHaptics&) = delete;

    // What the actuators play from now on.
    void SetVoices(const HapticsVoices& voices);
    // A thump on one side (see HapticsSynth::Kick).
    void Kick(size_t side, float strength);

    // False once the device went away (the pad was unplugged); it then stays silent for good.
    bool IsRunning() const;
    std::string DeviceName() const;
    uint32_t SampleRate() const;
    uint32_t Channels() const;

  private:
    struct Impl;
    explicit GamepadHaptics(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

// Whether a playback device's name is a DualSense's (or a DualSense Edge's).
bool IsDualSenseAudioDeviceName(const std::string& name);
}
