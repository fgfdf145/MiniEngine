#include "haptics_synth.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace me
{

namespace
{
constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;

// Levels glide to a new frame's values over about this long.
constexpr float kGlideSeconds = 0.012f;
// The filters follow their gliding centres in blocks of this many frames.
constexpr uint64_t kBlockFrames = 64;

// The engine: its firing frequency with a second harmonic, each firing a little stronger or weaker than
// the last as combustion varies. At the limiter the ignition cuts in and out at kLimiterHz.
constexpr float kEngineSecondHarmonic = 0.35f;
constexpr float kEngineWaveformPeak = 1.3f;
constexpr float kEngineCycleVariation = 0.12f;
constexpr float kLimiterHz = 16.0f;
constexpr float kLimiterCutGain = 0.1f;
constexpr float kLimiterGlideSeconds = 0.003f;

// The road's grain: noise in a wide band that rises with speed.
constexpr float kRoadMinHz = 20.0f;
constexpr float kRoadHzPerMetrePerSecond = 6.0f;
constexpr float kRoadMaxHz = 250.0f;
constexpr float kRoadQ = 0.7f;
// A sliding tyre: a narrower band, juddering as the rubber sticks and lets go.
constexpr float kSlipQ = 2.5f;
constexpr float kSlipMinHz = 40.0f;
constexpr float kSlipMaxHz = 400.0f;
constexpr float kJudderHz = 24.0f;
constexpr float kJudderDepth = 0.45f;
// Noise at amplitude 1 has this RMS, so its peaks reach about full scale.
constexpr float kNoiseRms = 0.33f;

// A thump: a low knock that dies away.
constexpr float kKickHz = 45.0f;
constexpr float kKickDecaySeconds = 0.035f;
constexpr float kKickSilent = 1.0e-4f;

float Saturate(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

float Wrap(float phase)
{
    return phase - std::floor(phase);
}
}

HapticsSynth::HapticsSynth(uint32_t sampleRate)
    : m_sampleRate(std::max(sampleRate, 1000u))
{
}

void HapticsSynth::SetVoices(const HapticsVoices& voices)
{
    m_target = voices;
}

void HapticsSynth::Kick(size_t side, float strength)
{
    if (side >= m_sides.size())
    {
        return;
    }
    Side& target = m_sides[side];
    const float amplitude = Saturate(strength);
    if (amplitude <= target.kick)
    {
        return;
    }
    // From silence the knock starts at its zero crossing, so it does not click.
    if (target.kick < kKickSilent)
    {
        target.kickPhase = 0.0f;
    }
    target.kick = amplitude;
}

HapticsSynth::BandCoefficients HapticsSynth::Band(float centreHz, float q) const
{
    const float sampleRate = static_cast<float>(m_sampleRate);
    const float centre = std::clamp(centreHz, 1.0f, 0.45f * sampleRate);
    BandCoefficients coefficients;
    const float g = std::tan(std::numbers::pi_v<float> * centre / sampleRate);
    coefficients.k = 1.0f / q;
    coefficients.a1 = 1.0f / (1.0f + g * (g + coefficients.k));
    coefficients.a2 = g * coefficients.a1;
    coefficients.a3 = g * coefficients.a2;
    // The band (unity gain at its centre) passes pi/2 times its width of white noise, whose variance is
    // 1/3 for uniform samples over [-1, 1].
    const float passedShare = std::numbers::pi_v<float> * centre / (q * sampleRate);
    coefficients.normalize = 1.0f / std::sqrt(passedShare / 3.0f);
    return coefficients;
}

float HapticsSynth::Filter(NoiseBand& band, const BandCoefficients& coefficients, float input) const
{
    const float v3 = input - band.ic2;
    const float v1 = coefficients.a1 * band.ic1 + coefficients.a2 * v3;
    const float v2 = band.ic2 + coefficients.a2 * band.ic1 + coefficients.a3 * v3;
    band.ic1 = 2.0f * v1 - band.ic1;
    band.ic2 = 2.0f * v2 - band.ic2;
    return coefficients.k * v1 * coefficients.normalize;
}

float HapticsSynth::Noise()
{
    // xorshift32: cheap, and white enough for a hand to feel.
    m_noise ^= m_noise << 13;
    m_noise ^= m_noise >> 17;
    m_noise ^= m_noise << 5;
    return static_cast<float>(m_noise) * (2.0f / 4294967296.0f) - 1.0f;
}

void HapticsSynth::Render(float* out, uint64_t frameCount, uint32_t stride)
{
    const float sampleRate = static_cast<float>(m_sampleRate);
    const float secondsPerSample = 1.0f / sampleRate;
    const float glide = 1.0f - std::exp(-secondsPerSample / kGlideSeconds);
    const float limiterGlide = 1.0f - std::exp(-secondsPerSample / kLimiterGlideSeconds);
    const float kickDecay = std::exp(-secondsPerSample / kKickDecaySeconds);
    const float targetEngineHz = std::max(m_target.engineHz, 0.0f);
    const float targetEngine = Saturate(m_target.engineAmplitude);
    const float targetRoadSpeed = std::max(m_target.roadSpeed, 0.0f);
    const float targetSlipHz = std::clamp(m_target.slipHz, kSlipMinHz, kSlipMaxHz);

    for (uint64_t blockStart = 0; blockStart < frameCount; blockStart += kBlockFrames)
    {
        const uint64_t blockEnd = std::min(blockStart + kBlockFrames, frameCount);
        const BandCoefficients road =
            Band(std::min(kRoadMinHz + kRoadHzPerMetrePerSecond * m_roadSpeed, kRoadMaxHz), kRoadQ);
        const BandCoefficients slip = Band(m_slipHz, kSlipQ);

        for (uint64_t frame = blockStart; frame < blockEnd; ++frame)
        {
            // An engine that has stopped keeps its pitch while it fades, rather than sliding down to 0 Hz.
            if (targetEngineHz > 0.0f)
            {
                m_engineHz += (targetEngineHz - m_engineHz) * glide;
            }
            m_engine += (targetEngine - m_engine) * glide;
            m_roadSpeed += (targetRoadSpeed - m_roadSpeed) * glide;
            m_slipHz += (targetSlipHz - m_slipHz) * glide;

            m_enginePhase += m_engineHz * secondsPerSample;
            if (m_enginePhase >= 1.0f)
            {
                m_enginePhase = Wrap(m_enginePhase);
                m_engineCycleGain = 1.0f + kEngineCycleVariation * Noise();
            }
            m_limiterPhase = m_target.limiter ? Wrap(m_limiterPhase + kLimiterHz * secondsPerSample) : 0.0f;
            const float limiterTarget = m_target.limiter && m_limiterPhase >= 0.5f ? kLimiterCutGain : 1.0f;
            m_limiterGain += (limiterTarget - m_limiterGain) * limiterGlide;

            const float engineWave =
                (std::sin(kTwoPi * m_enginePhase) + kEngineSecondHarmonic * std::sin(2.0f * kTwoPi * m_enginePhase)) / kEngineWaveformPeak;
            const float engine = m_engine * m_engineCycleGain * m_limiterGain * engineWave;

            m_judderPhase = Wrap(m_judderPhase + kJudderHz * secondsPerSample);
            const float judder = 1.0f - kJudderDepth + kJudderDepth * std::sin(kTwoPi * m_judderPhase);

            for (size_t sideIndex = 0; sideIndex < m_sides.size(); ++sideIndex)
            {
                Side& side = m_sides[sideIndex];
                side.road += (Saturate(m_target.roadAmplitude[sideIndex]) - side.road) * glide;
                side.slip += (Saturate(m_target.slipAmplitude[sideIndex]) - side.slip) * glide;

                float sample = engine;
                sample += side.road * kNoiseRms * Filter(side.roadBand, road, Noise());
                sample += side.slip * judder * kNoiseRms * Filter(side.slipBand, slip, Noise());
                if (side.kick >= kKickSilent)
                {
                    sample += side.kick * std::sin(kTwoPi * side.kickPhase);
                    side.kickPhase = Wrap(side.kickPhase + kKickHz * secondsPerSample);
                    side.kick *= kickDecay;
                }
                else
                {
                    side.kick = 0.0f;
                }
                // Several voices at once round off at full scale rather than clip.
                out[frame * stride + sideIndex] = std::tanh(sample);
            }
        }
    }
}
}
