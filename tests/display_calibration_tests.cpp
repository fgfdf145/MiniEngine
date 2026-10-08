#include <engine/renderer/display_calibration.h>
#include <engine/renderer/spirv_patch.h>

#include <glm/glm.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// The calibration's shader code, compiled as C++ (display_calibration.glsl is written in the subset of
// GLSL that GLM also accepts), so the test exercises the same source glslc compiles.
namespace shader
{
using namespace glm;
#include <shaders/vulkan/hdr_output.glsl>
#include <shaders/vulkan/display_calibration.glsl>
}

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool Near(float value, float expected, float relative)
{
    return std::abs(value - expected) <= relative * std::max(std::abs(expected), 1e-6f);
}

// The engine's PQ and the shader's agree, and decoding undoes encoding across the range.
void PqRoundTrips()
{
    for (const float nits : {0.001f, 0.05f, 1.0f, 100.0f, 203.0f, 600.0f, 1000.0f, 4000.0f, 10000.0f})
    {
        Require(Near(PqFromNits(nits), shader::PqEncodeNits(nits), 1e-5f), "engine and shader PQ agree at " + std::to_string(nits));
        Require(Near(NitsFromPq(PqFromNits(nits)), nits, 1e-3f), "the engine's PQ round trips at " + std::to_string(nits));
        Require(Near(shader::PqDecodeNits(shader::PqEncodeNits(nits)), nits, 1e-3f), "the shader's PQ round trips at " + std::to_string(nits));
    }
    Require(NitsFromPq(0.0f) == 0.0f && Near(NitsFromPq(1.0f), 10000.0f, 1e-5f), "PQ's ends");
}

// BT.2390's lift puts black on the floor, leaves highlights alone, keeps the order and does nothing
// without a floor.
void BlackLiftLandsOnTheFloor()
{
    const float floorNits = 0.05f;
    Require(Near(shader::LiftToBlackFloorNits(0.0f, floorNits), floorNits, 1e-3f), "0 cd/m^2 lands on the floor");
    // BT.2390's (1 - E)^4 falloff: +0.16 % at 1000 cd/m^2 and +2.7 % at 100 for a 0.05 floor.
    Require(Near(shader::LiftToBlackFloorNits(1000.0f, floorNits), 1000.0f, 3e-3f), "1000 cd/m^2 stays");
    Require(Near(shader::LiftToBlackFloorNits(100.0f, floorNits), 100.0f, 3e-2f), "100 cd/m^2 barely moves");
    float previous = -1.0f;
    for (float nits = 0.0f; nits < 50.0f; nits += 0.01f)
    {
        const float lifted = shader::LiftToBlackFloorNits(nits, floorNits);
        Require(lifted > previous, "the lift keeps the order");
        Require(lifted >= nits, "the lift only brightens");
        previous = lifted;
    }
    Require(shader::LiftToBlackFloorNits(0.3f, 0.0f) == 0.3f, "no floor, no lift");
}

// The SDR correction maps the signal's ends to the chosen white and black and is the identity at 1 / 0.
void SdrCorrectionMapsTheSignalEnds()
{
    for (const float linear : {0.0f, 0.001f, 0.18f, 0.5f, 1.0f})
    {
        Require(Near(shader::CorrectSdrSignal(linear, 1.0f, 0.0f), linear, 1e-4f), "white 1 and black 0 change nothing");
    }
    Require(Near(shader::CorrectSdrSignal(1.0f, 0.9f, 0.0f), shader::SrgbDecode(0.9f), 1e-5f), "white goes to the white signal");
    Require(Near(shader::CorrectSdrSignal(0.0f, 1.0f, 0.06f), shader::SrgbDecode(0.06f), 1e-5f), "black goes to the black signal");
    Require(shader::CorrectSdrSignal(0.2f, 0.9f, 0.05f) > shader::CorrectSdrSignal(0.19f, 0.9f, 0.05f), "the correction keeps the order");
}

void SaturationScalesChromaOnly()
{
    const glm::vec3 color(0.8f, 0.3f, 0.1f);
    const glm::vec3 same = shader::AdjustSaturation(color, 1.0f);
    Require(glm::all(glm::lessThan(glm::abs(same - color), glm::vec3(1e-6f))), "saturation 1 changes nothing");
    const glm::vec3 grey = shader::AdjustSaturation(color, 0.0f);
    const float luminance = glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
    Require(Near(grey.x, luminance, 1e-5f) && Near(grey.y, luminance, 1e-5f) && Near(grey.z, luminance, 1e-5f), "saturation 0 is the luminance");
    // A colour no channel of which goes negative at 1.5 (the clamp at 0 would add luminance).
    const glm::vec3 muted(0.6f, 0.4f, 0.3f);
    const glm::vec3 vivid = shader::AdjustSaturation(muted, 1.5f);
    const glm::vec3 weights(0.2126f, 0.7152f, 0.0722f);
    Require(Near(glm::dot(vivid, weights), glm::dot(muted, weights), 1e-4f), "more saturation keeps the luminance");
}

// The HDR patterns: the ring at 10 000 (or 0 on the black screen), the trial level round it, a
// window of 10 % of the screen.
void HdrPatternsShowTheLevel()
{
    const float aspect = 16.0f / 9.0f;
    const glm::vec2 centre(0.0f, 0.0f);
    const glm::vec2 onRing(0.11f, 0.0f);
    const glm::vec2 corner(0.8f, 0.45f);
    using shader::CalibrationHdrNits;
    Require(CalibrationHdrNits(shader::CALIBRATION_HDR_FULL_FRAME, 600.0f, centre, aspect) == 600.0f, "full frame: the level inside the ring");
    Require(CalibrationHdrNits(shader::CALIBRATION_HDR_FULL_FRAME, 600.0f, corner, aspect) == 600.0f, "full frame: the level to the corners");
    Require(CalibrationHdrNits(shader::CALIBRATION_HDR_FULL_FRAME, 600.0f, onRing, aspect) == 10000.0f, "full frame: the ring at 10 000");
    Require(CalibrationHdrNits(shader::CALIBRATION_HDR_WINDOW, 600.0f, corner, aspect) == 0.0f, "window: black round it");
    Require(CalibrationHdrNits(shader::CALIBRATION_HDR_WINDOW, 600.0f, onRing, aspect) == 10000.0f, "window: the ring inside it");
    Require(CalibrationHdrNits(shader::CALIBRATION_HDR_BLACK, 0.02f, onRing, aspect) == 0.0f, "black: the ring at 0");
    Require(CalibrationHdrNits(shader::CALIBRATION_HDR_BLACK, 0.02f, corner, aspect) == 0.02f, "black: the level round it");

    // The window's area, sampled on a grid over the screen (width aspect, height 1).
    int lit = 0;
    int total = 0;
    for (int y = 0; y < 300; ++y)
    {
        for (int x = 0; x < 533; ++x)
        {
            const glm::vec2 p((static_cast<float>(x) + 0.5f) / 533.0f * aspect - 0.5f * aspect, (static_cast<float>(y) + 0.5f) / 300.0f - 0.5f);
            lit += CalibrationHdrNits(shader::CALIBRATION_HDR_WINDOW, 600.0f, p, aspect) > 0.0f ? 1 : 0;
            ++total;
        }
    }
    const float share = static_cast<float>(lit) / static_cast<float>(total);
    Require(std::abs(share - 0.1f) < 0.005f, "the window covers 10 % of the screen, got " + std::to_string(share));
}

void SdrCheckerboardsAlternate()
{
    const float cell = 1.0f / 16.0f;
    const glm::vec2 a(-0.25f + 0.5f * cell, -0.25f + 0.5f * cell);
    const glm::vec2 b = a + glm::vec2(cell, 0.0f);
    const float brightA = shader::CalibrationSdrSignal(shader::CALIBRATION_SDR_BRIGHT, a);
    const float brightB = shader::CalibrationSdrSignal(shader::CALIBRATION_SDR_BRIGHT, b);
    Require(brightA != brightB && std::max(brightA, brightB) == 1.0f && std::min(brightA, brightB) == shader::kCalibrationBrightCheck, "bright cells alternate 1 / 0.96");
    const float darkA = shader::CalibrationSdrSignal(shader::CALIBRATION_SDR_DARK, a);
    const float darkB = shader::CalibrationSdrSignal(shader::CALIBRATION_SDR_DARK, b);
    Require(darkA != darkB && std::min(darkA, darkB) == 0.0f && std::max(darkA, darkB) == shader::kCalibrationDarkCheck, "dark cells alternate 0 / 0.04");
    Require(shader::CalibrationSdrSignal(shader::CALIBRATION_SDR_BRIGHT, glm::vec2(0.6f, 0.0f)) == 1.0f, "white round the board");
    Require(shader::CalibrationSdrSignal(shader::CALIBRATION_SDR_DARK, glm::vec2(0.6f, 0.0f)) == 0.0f, "black round the board");
}

void WedgeStepsByStops()
{
    // The middle of the thirteen steps is paper white.
    Require(shader::CalibrationSampleWedge(glm::vec2(6.5f / 13.0f, 0.25f)).x == 2.5f, "the middle step is paper white");
    Require(shader::CalibrationSampleWedge(glm::vec2(7.5f / 13.0f, 0.25f)).x == 5.0f, "the next step is a stop up");
    const glm::vec3 sun = shader::CalibrationSampleSky(glm::vec2(0.68f, 0.28f), 16.0f / 9.0f);
    Require(sun.x > 1000.0f, "the sun is far above any display");
}

// Until a calibration, the display's figures; after, the calibration's; UI white from the OS unless
// overridden; the ranges held.
void ResolveUsesCalibrationThenReport()
{
    DisplayReport report;
    report.known = true;
    report.hdrEnabled = true;
    report.maxLuminance = 600.0f;
    report.maxFullFrameLuminance = 400.0f;
    report.minLuminance = 0.005f;
    report.sdrWhiteNits = 480.0f;

    DisplaySettings settings;
    DisplayOutput output = ResolveDisplayOutput(settings, report, true);
    Require(output.hdr && output.maxLuminance == 600.0f && output.maxFullFrameLuminance == 400.0f && output.minLuminance == 0.005f, "the display's figures");
    Require(output.uiWhiteNits == 480.0f, "UI white is Windows' SDR content brightness");

    settings.calibrated = true;
    settings.maxLuminance = 750.0f;
    settings.maxFullFrameLuminance = 350.0f;
    settings.minLuminance = 0.02f;
    settings.uiWhiteNits = 250.0f;
    output = ResolveDisplayOutput(settings, report, true);
    Require(output.maxLuminance == 750.0f && output.maxFullFrameLuminance == 350.0f && output.minLuminance == 0.02f, "the calibration");
    Require(output.uiWhiteNits == 250.0f, "the override");

    settings.maxLuminance = 50000.0f;
    settings.minLuminance = -1.0f;
    settings.sdrWhite = 0.1f;
    settings.saturation = 9.0f;
    output = ResolveDisplayOutput(settings, report, false);
    Require(output.maxLuminance == kMaxCalibrationPeakNits && output.minLuminance == 0.0f, "luminance ranges");
    Require(output.sdrWhite == 0.5f && output.saturation == 2.0f, "SDR and saturation ranges");

    const DisplayOutput unknown = ResolveDisplayOutput(DisplaySettings{}, DisplayReport{}, true);
    Require(unknown.maxLuminance == 1000.0f && unknown.uiWhiteNits == kDefaultUiWhiteNits, "nothing reported: 1000 cd/m^2 and BT.2408's white");
}

void HdrFollowsTheOsOnAuto()
{
    DisplayReport hdrOn;
    hdrOn.known = true;
    hdrOn.hdrEnabled = true;
    DisplaySettings settings;
    Require(WantsHdrOutput(settings, hdrOn, false), "Auto follows Windows into HDR");
    Require(!WantsHdrOutput(settings, hdrOn, true), "a scripted run on Auto stays SDR");
    Require(!WantsHdrOutput(settings, DisplayReport{}, false), "Auto stays SDR when Windows is not in HDR");
    settings.outputMode = DisplayOutputMode::Hdr;
    Require(WantsHdrOutput(settings, DisplayReport{}, true), "HDR is asked for whatever the OS says");
    settings.outputMode = DisplayOutputMode::Sdr;
    Require(!WantsHdrOutput(settings, hdrOn, false), "SDR stays SDR");
}

// A hand-made module: OpDecorate %7 SpecId 0, OpTypeFloat %6 32, OpSpecConstant %6 %7 203.0, and an
// unrelated OpConstant 203.0 that must stay.
void SpecConstantIsPatched()
{
    const uint32_t f203 = std::bit_cast<uint32_t>(203.0f);
    std::vector<uint32_t> module = {
        0x07230203u, 0x00010000u, 0u, 16u, 0u,
        (4u << 16) | 71u, 7u, 1u, 0u,
        (3u << 16) | 22u, 6u, 32u,
        (4u << 16) | 50u, 6u, 7u, f203,
        (4u << 16) | 43u, 6u, 8u, f203};
    Require(PatchSpecConstantFloat(module, 0, 480.0f), "the constant is found");
    Require(std::bit_cast<float>(module[15]) == 480.0f, "its default is the new value");
    Require(module[19] == f203, "an ordinary constant is left alone");
    Require(!PatchSpecConstantFloat(module, 1, 1.0f), "another SpecId is not there");
    std::vector<uint32_t> broken = {0x07230203u, 0u, 0u, 16u, 0u, (9u << 16) | 71u};
    Require(!PatchSpecConstantFloat(broken, 0, 1.0f), "a truncated module is refused");
}
}

int main()
{
    try
    {
        PqRoundTrips();
        BlackLiftLandsOnTheFloor();
        SdrCorrectionMapsTheSignalEnds();
        SaturationScalesChromaOnly();
        HdrPatternsShowTheLevel();
        SdrCheckerboardsAlternate();
        WedgeStepsByStops();
        ResolveUsesCalibrationThenReport();
        HdrFollowsTheOsOnAuto();
        SpecConstantIsPatched();
    }
    catch (const std::exception& error)
    {
        std::cerr << "display calibration tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "display calibration tests passed\n";
    return 0;
}
