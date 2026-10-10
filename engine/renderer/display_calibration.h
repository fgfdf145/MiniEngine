#pragma once

// HDR output and its calibration (docs/design/2026-10-10-hdr-calibration-design.md): the PS5's
// Adjust HDR luminances, the UI and paper white that follow Windows' SDR content brightness, the
// patterns the calibration screen shows, and how they combine with what the OS reports into the
// values the output uses.

#include <cstdint>

namespace me
{

// What the calibration screen asks the tone mapping pass to show instead of the scene. The values
// must match CALIBRATION_* in shaders/vulkan/display_calibration.slang.
enum class CalibrationPattern : uint32_t
{
    None = 0,
    // In absolute cd/m^2 past every adjustment: the whole screen at the level, a ring at
    // 10 000 cd/m^2.
    HdrFullFrame = 1,
    // Black with a centred square of 10 % of the screen at the level, the ring at 10 000 inside it.
    HdrWindow = 2,
    // The whole screen at the level, the ring at 0 cd/m^2.
    HdrBlack = 3
};

// The calibration the user keeps (saved with the render settings, group "display"). Whether HDR is
// presented at all is RenderDebugSettings::hdrOutput.
struct DisplaySettings
{
    bool operator==(const DisplaySettings&) const = default;

    // Whether the luminances below were set on the calibration screen. Until then the output uses
    // what the display reports.
    bool calibrated = false;
    // In cd/m^2: where the display clips a small highlight (GT7's HDR curve peaks here), where it
    // clips a full white screen, and the darkest level it still tells from black.
    float maxLuminance = 1000.0f;
    float maxFullFrameLuminance = 1000.0f;
    float minLuminance = 0.0f;
    // Where UI white (SDR 1.0) shows in HDR, in cd/m^2; 0 follows the OS's SDR content brightness.
    float uiWhiteNits = 0.0f;
    // Where the scene's paper white (GT7's 250 cd/m^2 SDR white) shows in HDR, in cd/m^2; 0 follows
    // the UI white, so the scene's midtones are as bright as in SDR on this desktop and only the
    // highlights go further. 250 is GT7's absolute scale.
    float paperWhiteNits = 0.0f;
};

// What the calibration screen shows this frame (never saved).
struct DisplayCalibrationView
{
    bool operator==(const DisplayCalibrationView&) const = default;

    CalibrationPattern pattern = CalibrationPattern::None;
    // The trial level in cd/m^2.
    float level = 0.0f;
};

// What the OS says about the display (platform::display::DisplayHdrInfo, without the platform).
struct DisplayReport
{
    bool known = false;
    bool hdrEnabled = false;
    float maxLuminance = 0.0f;
    float maxFullFrameLuminance = 0.0f;
    float minLuminance = 0.0f;
    float sdrWhiteNits = 80.0f;
};

// The values the output uses this frame.
struct DisplayOutput
{
    bool operator==(const DisplayOutput&) const = default;

    // The swapchain is HDR10.
    bool hdr = false;
    float maxLuminance = 1000.0f;
    float maxFullFrameLuminance = 1000.0f;
    float minLuminance = 0.0f;
    float uiWhiteNits = 203.0f;
    // The scene's paper white in HDR (see DisplaySettings::paperWhiteNits).
    float paperWhiteNits = 250.0f;
};

// BT.2408's graphics white: UI white when nothing says better.
constexpr float kDefaultUiWhiteNits = 203.0f;
// The ranges the settings are held to.
constexpr float kMinCalibrationPeakNits = 100.0f;
constexpr float kMaxCalibrationPeakNits = 10000.0f;
constexpr float kMaxCalibrationBlackNits = 5.0f;
constexpr float kMinUiWhiteNits = 80.0f;
constexpr float kMaxUiWhiteNits = 1000.0f;
// GT7's SDR paper white: the scene's white at an HDR paper white of this many cd/m^2 is GT7's own.
constexpr float kGt7PaperWhiteNits = 250.0f;

// The calibrated values, or what the display reports until there are some, held to their ranges;
// hdr is whether the swapchain is HDR10.
DisplayOutput ResolveDisplayOutput(const DisplaySettings& settings, const DisplayReport& report, bool hdr);

// The scale HDR output applies to the scene before GT7's curve so its paper white lands at
// output.paperWhiteNits (1 in SDR).
float HdrPaperWhiteScale(const DisplayOutput& output);

// The PQ signal of the display's black floor that hdr_ui_encode.frag lifts the frame onto (BT.2390);
// 0 (no lift) in SDR, without a floor, and while the calibration screen shows a pattern, which must
// reach the display at its absolute level.
float HdrBlackFloorPq(const DisplayOutput& output, CalibrationPattern pattern);

// SMPTE ST 2084: cd/m^2 to the PQ signal in [0, 1] and back (hdr_output.slang's, compiled as C++). The
// calibration's levels step evenly in PQ, as the eye sees them.
float PqFromNits(float nits);
float NitsFromPq(float pq);
}
