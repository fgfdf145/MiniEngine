#include <engine/audio/gamepad_haptics.h>
#include <engine/audio/haptics_synth.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
constexpr uint32_t kSampleRate = 48000;

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// Interleaved left and right actuators.
std::vector<float> Render(HapticsSynth& synth, float seconds)
{
    const uint64_t frames = static_cast<uint64_t>(seconds * static_cast<float>(synth.SampleRate()));
    std::vector<float> samples(frames * 2, 0.0f);
    synth.Render(samples.data(), frames, 2);
    return samples;
}

float Rms(const std::vector<float>& samples, size_t side, size_t firstFrame = 0, size_t lastFrame = SIZE_MAX)
{
    lastFrame = std::min(lastFrame, samples.size() / 2);
    double sum = 0.0;
    for (size_t frame = firstFrame; frame < lastFrame; ++frame)
    {
        sum += static_cast<double>(samples[frame * 2 + side]) * samples[frame * 2 + side];
    }
    return lastFrame > firstFrame ? static_cast<float>(std::sqrt(sum / static_cast<double>(lastFrame - firstFrame))) : 0.0f;
}

void SilentWithNothingToPlay()
{
    HapticsSynth synth(kSampleRate);
    const std::vector<float> samples = Render(synth, 0.2f);
    Require(std::all_of(samples.begin(), samples.end(), [](float sample) { return sample == 0.0f; }), "no voices is silence");
}

void EngineBeatsAtItsFiringFrequency()
{
    HapticsSynth synth(kSampleRate);
    HapticsVoices voices;
    voices.engineHz = 100.0f;
    voices.engineAmplitude = 1.0f;
    synth.SetVoices(voices);
    Render(synth, 0.2f); // the glide settles

    const std::vector<float> samples = Render(synth, 1.0f);
    int crossings = 0;
    for (size_t frame = 1; frame < samples.size() / 2; ++frame)
    {
        if ((samples[(frame - 1) * 2] < 0.0f) != (samples[frame * 2] < 0.0f))
        {
            ++crossings;
        }
        Require(samples[frame * 2] == samples[frame * 2 + 1], "the engine is the same in both hands");
    }
    Require(std::abs(crossings - 200) <= 2, "100 Hz crosses zero 200 times a second, got " + std::to_string(crossings));
    Require(Rms(samples, 0) > 0.3f, "the engine at full amplitude is strong");
}

void LimiterChopsTheEngine()
{
    HapticsSynth synth(kSampleRate);
    HapticsVoices voices;
    voices.engineHz = 300.0f;
    voices.engineAmplitude = 0.8f;
    voices.limiter = true;
    synth.SetVoices(voices);
    Render(synth, 0.1f);
    const std::vector<float> samples = Render(synth, 0.5f);
    // Windows of 1/64 s: a 16 Hz cut leaves some loud and some nearly still.
    const size_t window = kSampleRate / 64;
    float loudest = 0.0f;
    float quietest = 1.0f;
    for (size_t first = 0; first + window <= samples.size() / 2; first += window)
    {
        const float rms = Rms(samples, 0, first, first + window);
        loudest = std::max(loudest, rms);
        quietest = std::min(quietest, rms);
    }
    Require(loudest > 4.0f * quietest, "the limiter cuts the engine in and out");
}

void RoadAndSlipStayOnTheirSide()
{
    HapticsSynth synth(kSampleRate);
    HapticsVoices voices;
    voices.roadAmplitude = {1.0f, 0.0f};
    voices.roadSpeed = 20.0f;
    voices.slipAmplitude = {0.0f, 0.0f};
    synth.SetVoices(voices);
    Render(synth, 0.1f);
    std::vector<float> samples = Render(synth, 1.0f);
    const float road = Rms(samples, 0);
    Require(road > 0.2f && road < 0.45f, "the road at full amplitude is about a third of full scale, got " + std::to_string(road));
    Require(Rms(samples, 1) == 0.0f, "the other side's road is still");

    voices.roadAmplitude = {0.0f, 0.0f};
    voices.slipAmplitude = {0.0f, 0.6f};
    voices.slipHz = 150.0f;
    synth.SetVoices(voices);
    Render(synth, 0.2f);
    samples = Render(synth, 1.0f);
    Require(Rms(samples, 0) < 0.005f, "the road fades out");
    const float slip = Rms(samples, 1);
    Require(slip > 0.1f && slip < 0.3f, "a sliding tyre buzzes on its side, got " + std::to_string(slip));
}

void KicksKnockAndDieAway()
{
    HapticsSynth synth(kSampleRate);
    synth.Kick(1, 1.0f);
    const std::vector<float> samples = Render(synth, 0.3f);
    float peak = 0.0f;
    for (size_t frame = 0; frame < kSampleRate / 50; ++frame)
    {
        peak = std::max(peak, std::abs(samples[frame * 2 + 1]));
    }
    Require(peak > 0.5f, "a full kick knocks hard at once");
    Require(std::abs(samples[1]) < 0.01f, "it starts at its zero crossing");
    Require(Rms(samples, 1, kSampleRate / 5) < 0.01f, "it has died away within a fifth of a second");
    Require(Rms(samples, 0) == 0.0f, "the other side is still");
}

void StaysInRangeAndInItsChannels()
{
    HapticsSynth synth(kSampleRate);
    HapticsVoices voices;
    voices.engineHz = 80.0f;
    voices.engineAmplitude = 1.0f;
    voices.roadAmplitude = {1.0f, 1.0f};
    voices.roadSpeed = 60.0f;
    voices.slipAmplitude = {1.0f, 1.0f};
    synth.SetVoices(voices);
    synth.Kick(0, 1.0f);
    synth.Kick(1, 1.0f);

    // Four channels, the actuators on the last two, as the pad has them.
    constexpr uint32_t kChannels = 4;
    const uint64_t frames = kSampleRate / 2;
    std::vector<float> samples(frames * kChannels, 7.0f);
    synth.Render(samples.data() + 2, frames, kChannels);
    for (uint64_t frame = 0; frame < frames; ++frame)
    {
        Require(samples[frame * kChannels] == 7.0f && samples[frame * kChannels + 1] == 7.0f, "the speaker's channels are left alone");
        Require(std::abs(samples[frame * kChannels + 2]) <= 1.0f && std::abs(samples[frame * kChannels + 3]) <= 1.0f, "everything at once stays within full scale");
    }
}

void NamesTheDualSense()
{
    Require(IsDualSenseAudioDeviceName("Speakers (DualSense Wireless Controller)"), "Windows' name for it");
    Require(IsDualSenseAudioDeviceName("Speakers (DualSense Edge Wireless Controller)"), "the Edge");
    Require(IsDualSenseAudioDeviceName("Wireless Controller"), "older drivers' name");
    Require(!IsDualSenseAudioDeviceName("Speakers (Realtek(R) Audio)"), "a sound card is not one");
}

// --device: opens a connected DualSense (USB) and plays each voice in turn, to feel them by hand.
int PlayOnTheDevice()
{
    std::string error;
    std::unique_ptr<GamepadHaptics> haptics = GamepadHaptics::Open(error);
    if (!haptics)
    {
        std::cerr << "No haptics: " << error << '\n';
        return 1;
    }
    std::cout << "Playing on '" << haptics->DeviceName() << "' (" << haptics->SampleRate() << " Hz, " << haptics->Channels() << " channels)\n";
    const auto hold = [&](const char* what, const HapticsVoices& voices, float seconds)
    {
        std::cout << "  " << what << '\n';
        haptics->SetVoices(voices);
        std::this_thread::sleep_for(std::chrono::duration<float>(seconds));
    };

    HapticsVoices voices;
    for (float rpm = 1000.0f; rpm <= 7000.0f; rpm += 1000.0f)
    {
        voices.engineHz = rpm / 60.0f * 3.0f;
        voices.engineAmplitude = 0.15f + 0.3f * (rpm - 1000.0f) / 6000.0f;
        hold(("engine " + std::to_string(static_cast<int>(rpm)) + " rpm").c_str(), voices, 0.7f);
    }
    voices.limiter = true;
    hold("limiter", voices, 1.5f);
    voices = HapticsVoices{};
    voices.roadSpeed = 25.0f;
    voices.roadAmplitude = {0.15f, 0.15f};
    hold("smooth road", voices, 1.5f);
    voices.roadAmplitude = {0.7f, 0.2f};
    hold("rough road under the left wheels", voices, 1.5f);
    voices.roadAmplitude = {0.1f, 0.1f};
    for (int bump = 0; bump < 4; ++bump)
    {
        haptics->Kick(bump % 2, 0.9f);
        hold(bump % 2 == 0 ? "bump left" : "bump right", voices, 0.4f);
    }
    voices.slipAmplitude = {0.5f, 0.5f};
    voices.slipHz = 120.0f;
    hold("tyres sliding", voices, 1.5f);
    hold("quiet", HapticsVoices{}, 0.3f);
    std::cout << (haptics->IsRunning() ? "done\n" : "the device stopped\n");
    return haptics->IsRunning() ? 0 : 1;
}
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "--device")
    {
        return PlayOnTheDevice();
    }
    try
    {
        SilentWithNothingToPlay();
        EngineBeatsAtItsFiringFrequency();
        LimiterChopsTheEngine();
        RoadAndSlipStayOnTheirSide();
        KicksKnockAndDieAway();
        StaysInRangeAndInItsChannels();
        NamesTheDualSense();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "haptics synth tests passed\n";
    return 0;
}
