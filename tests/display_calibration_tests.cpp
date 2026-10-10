#include <engine/renderer/display_calibration.h>
#include <engine/renderer/shader_cpp_compat.h>

#include <glm/glm.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

// The calibration's shader code, compiled as C++ (hdr_output.slang and display_calibration.slang are
// written in the subset of Slang that C++ with GLM also accepts), so the test exercises the same
// source slangc compiles.
namespace shader
{
using namespace glm;
using namespace me::shader_cpp;
#include <shaders/vulkan/hdr_output.slang>
#include <shaders/vulkan/display_calibration.slang>
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

// The patterns: the ring at 10 000 (or 0 on the black screen), the trial level round it, a window of
// 10 % of the screen.
void PatternsShowTheLevel()
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
    Require(static_cast<uint32_t>(CalibrationPattern::HdrBlack) == shader::CALIBRATION_HDR_BLACK &&
                static_cast<uint32_t>(CalibrationPattern::GtPeak) == shader::CALIBRATION_GT_PEAK,
            "the pattern numbers match the shader's");

    // Gran Turismo's checkerboard: inside the same window, half its cells at 10 000 cd/m^2 and half
    // at the level, neighbours different; black outside.
    int bright = 0;
    int atLevel = 0;
    for (int y = 0; y < 300; ++y)
    {
        for (int x = 0; x < 533; ++x)
        {
            const glm::vec2 p((static_cast<float>(x) + 0.5f) / 533.0f * aspect - 0.5f * aspect, (static_cast<float>(y) + 0.5f) / 300.0f - 0.5f);
            const float nits = CalibrationHdrNits(shader::CALIBRATION_GT_PEAK, 600.0f, p, aspect);
            bright += nits == shader::kCalibrationRingNits ? 1 : 0;
            atLevel += nits == 600.0f ? 1 : 0;
        }
    }
    const float brightShare = static_cast<float>(bright) / static_cast<float>(bright + atLevel);
    Require(std::abs(static_cast<float>(bright + atLevel) / static_cast<float>(total) - 0.1f) < 0.005f, "the checkerboard fills the 10 % window");
    Require(std::abs(brightShare - 0.5f) < 0.03f, "half the checkerboard is the fixed white, got " + std::to_string(brightShare));
    const float halfSide = 0.5f * std::sqrt(0.1f * aspect);
    const float cell = halfSide / 4.0f;
    Require(CalibrationHdrNits(shader::CALIBRATION_GT_PEAK, 600.0f, glm::vec2(-halfSide + 0.5f * cell), aspect) !=
                CalibrationHdrNits(shader::CALIBRATION_GT_PEAK, 600.0f, glm::vec2(-halfSide + 1.5f * cell, -halfSide + 0.5f * cell), aspect),
            "neighbouring cells differ");
}

// Until a calibration, the display's figures; after, the calibration's; UI white from the OS unless
// overridden; paper white following it unless set; the ranges held.
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
    Require(output.paperWhiteNits == 480.0f, "the paper white follows the UI white");
    Require(std::abs(HdrPaperWhiteScale(output) - 480.0f / 250.0f) < 1e-6f, "the scene is lifted from GT7's 250 to it");
    Require(output.scenePeakNits == 600.0f, "the scene's peak follows the display's");

    settings.calibrated = true;
    settings.maxLuminance = 750.0f;
    settings.maxFullFrameLuminance = 350.0f;
    settings.minLuminance = 0.02f;
    settings.uiWhiteNits = 250.0f;
    output = ResolveDisplayOutput(settings, report, true);
    Require(output.maxLuminance == 750.0f && output.maxFullFrameLuminance == 350.0f && output.minLuminance == 0.02f, "the calibration");
    Require(output.scenePeakNits == 750.0f, "the scene's peak follows the calibrated 10 % window");
    settings.scenePeakNits = 1400.0f;
    Require(ResolveDisplayOutput(settings, report, true).scenePeakNits == 1400.0f, "Gran Turismo's checkerboard sets the scene's peak");
    settings.scenePeakNits = 0.0f;
    Require(output.uiWhiteNits == 250.0f, "the override");
    Require(output.paperWhiteNits == 250.0f, "the paper white follows the overridden UI white");
    settings.uiWhiteNits = 0.0f;
    settings.paperWhiteNits = 250.0f;
    output = ResolveDisplayOutput(settings, report, true);
    Require(output.paperWhiteNits == 250.0f && HdrPaperWhiteScale(output) == 1.0f, "250 is GT7's own scale, whatever the UI white");

    settings.maxLuminance = 50000.0f;
    settings.minLuminance = -1.0f;
    output = ResolveDisplayOutput(settings, report, false);
    Require(output.maxLuminance == kMaxCalibrationPeakNits && output.minLuminance == 0.0f, "luminance ranges");
    Require(HdrPaperWhiteScale(output) == 1.0f, "no paper white lift in SDR");

    const DisplayOutput unknown = ResolveDisplayOutput(DisplaySettings{}, DisplayReport{}, true);
    Require(unknown.maxLuminance == 1000.0f && unknown.uiWhiteNits == kDefaultUiWhiteNits, "nothing reported: 1000 cd/m^2 and BT.2408's white");
}
}

int main()
{
    try
    {
        PqRoundTrips();
        BlackLiftLandsOnTheFloor();
        PatternsShowTheLevel();
        ResolveUsesCalibrationThenReport();
    }
    catch (const std::exception& error)
    {
        std::cerr << "display calibration tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "display calibration tests passed\n";
    return 0;
}
