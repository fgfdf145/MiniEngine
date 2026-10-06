#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace me
{

// A gamepad's haptic actuators: one in each grip, left (0) and right (1).
inline constexpr size_t kHapticsSides = 2;

// What the actuators should play, as a few levels that change from frame to frame; HapticsSynth turns
// them into waveforms. Amplitudes run from 0 (still) to 1 (about as hard as an actuator goes).
struct HapticsVoices
{
    // The engine: its firing frequency (Hz) and how hard it shakes, the same in both hands. While
    // `limiter` is on the ignition cut chops it.
    float engineHz = 0.0f;
    float engineAmplitude = 0.0f;
    bool limiter = false;
    // The road under each side's tyres: a grain that runs finer (higher) the faster it passes (m/s).
    std::array<float, kHapticsSides> roadAmplitude{};
    float roadSpeed = 0.0f;
    // Each side's tyres sliding: a rough buzz about slipHz that judders as the rubber sticks and lets go.
    std::array<float, kHapticsSides> slipAmplitude{};
    float slipHz = 120.0f;

    bool operator==(const HapticsVoices&) const = default;
};

// Makes the actuators' waveforms from HapticsVoices, sample by sample. The levels glide to new values
// over a few milliseconds, so a frame's step in them is not a click in the hands. Not thread safe: the
// audio thread owns it.
class HapticsSynth
{
  public:
    explicit HapticsSynth(uint32_t sampleRate);

    uint32_t SampleRate() const
    {
        return m_sampleRate;
    }

    void SetVoices(const HapticsVoices& voices);
    // A thump on one side (a bump, a kerb, a gear change): a low knock that dies away within a tenth of
    // a second. strength is from 0 to 1; a weaker one while a stronger still rings is lost in it.
    void Kick(size_t side, float strength);

    // Writes frameCount frames of the left and right actuators, from -1 to 1: frame n's left sample at
    // out[n * stride] and its right one at out[n * stride + 1]. Other samples are left alone.
    void Render(float* out, uint64_t frameCount, uint32_t stride);

  private:
    // A bandpass (a state variable filter in its trapezoidal form) shaping white noise.
    struct NoiseBand
    {
        float ic1 = 0.0f;
        float ic2 = 0.0f;
    };
    struct BandCoefficients
    {
        float a1 = 0.0f;
        float a2 = 0.0f;
        float a3 = 0.0f;
        float k = 1.0f;
        // Takes the band of uniform white noise to an RMS of 1.
        float normalize = 0.0f;
    };
    struct Side
    {
        float road = 0.0f;
        float slip = 0.0f;
        NoiseBand roadBand;
        NoiseBand slipBand;
        float kick = 0.0f;
        float kickPhase = 0.0f;
    };

    BandCoefficients Band(float centreHz, float q) const;
    float Filter(NoiseBand& band, const BandCoefficients& coefficients, float input) const;
    float Noise();

    uint32_t m_sampleRate = 48000;
    HapticsVoices m_target;
    // The levels as they glide, and the oscillators' phases (in cycles).
    float m_engineHz = 0.0f;
    float m_engine = 0.0f;
    float m_enginePhase = 0.0f;
    float m_engineCycleGain = 1.0f;
    float m_limiterGain = 1.0f;
    float m_limiterPhase = 0.0f;
    float m_roadSpeed = 0.0f;
    float m_slipHz = 120.0f;
    float m_judderPhase = 0.0f;
    std::array<Side, kHapticsSides> m_sides{};
    uint32_t m_noise = 0x9E3779B9u;
};
}
