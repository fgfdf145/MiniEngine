#pragma once

// The display calibration (docs/design/2026-10-08-display-calibration-design.md): the PS5's Adjust
// HDR luminances, GT7's exposure and saturation and its SDR bright and dark corrections, the
// patterns the calibration screen shows, and how they combine with what the OS reports into the
// values the output uses.

#include <cstdint>

namespace me
{

// Whether the editor presents HDR10. Auto follows the OS (Windows' "Use HDR"), as GT7 follows the
// console's HDR setting. The values are what the settings files store.
enum class DisplayOutputMode : uint32_t
{
    Auto = 0,
    Hdr = 1,
    Sdr = 2
};

// What the calibration screen asks the tone mapping pass to show instead of the scene. The values
// must match CALIBRATION_* in shaders/vulkan/display_calibration.glsl.
enum class CalibrationPattern : uint32_t
{
    None = 0,
    // HDR, in absolute cd/m^2 past every adjustment: the whole screen at the level, a ring at
    // 10 000 cd/m^2.
    HdrFullFrame = 1,
    // Black with a centred square of 10 % of the screen at the level, the ring at 10 000 inside it.
    HdrWindow = 2,
    // The whole screen at the level, the ring at 0 cd/m^2.
    HdrBlack = 3,
    // SDR checkerboards through the signal correction the level stands for: 1.0 / 0.96 with the
    // level as white, 0.0 / 0.04 with it as black.
    SdrBright = 4,
    SdrDark = 5,
    // Synthetic test cards through the whole output path, in place of the scene.
    SampleWedge = 6,
    SampleSky = 7
};

// The calibration the user keeps (saved with the render settings, group "display").
struct DisplaySettings
{
    bool operator==(const DisplaySettings&) const = default;

    DisplayOutputMode outputMode = DisplayOutputMode::Auto;
    // Whether the luminances below were set on the calibration screen. Until then the output uses
    // what the display reports.
    bool calibrated = false;
    // In cd/m^2: where the display clips a small highlight (GT7's HDR curve peaks here), where it
    // clips a full white screen, and the darkest level it still tells from black.
    float maxLuminance = 1000.0f;
    float maxFullFrameLuminance = 1000.0f;
    float minLuminance = 0.0f;
    // GT7's Exposure and Saturation: EV added to the scene before the curve, and the chroma's scale.
    float exposureEv = 0.0f;
    float saturation = 1.0f;
    // SDR bright and dark section correction: the sRGB signal the scene's white and black go to.
    float sdrWhite = 1.0f;
    float sdrBlack = 0.0f;
    // Where UI white (SDR 1.0) shows in HDR, in cd/m^2; 0 follows the OS's SDR content brightness.
    float uiWhiteNits = 0.0f;
    // Where the scene's paper white (GT7's 250 cd/m^2 SDR white) shows in HDR, in cd/m^2; 0 follows
    // the UI white, so the scene's midtones are as bright as in SDR on this desktop and only the
    // highlights go further. 250 is GT7's absolute scale.
    float paperWhiteNits = 0.0f;
    // Starts frames at even intervals under HDR output (frame_pacing.h): without it the HDR swapchain
    // presents in bursts while the GPU is saturated.
    bool hdrFramePacing = true;
};

// What the calibration screen shows this frame (never saved).
struct DisplayCalibrationView
{
    bool operator==(const DisplayCalibrationView&) const = default;

    CalibrationPattern pattern = CalibrationPattern::None;
    // The trial level: cd/m^2 for the HDR patterns, an sRGB signal for the SDR ones.
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
    bool hdr = false;
    float maxLuminance = 1000.0f;
    float maxFullFrameLuminance = 1000.0f;
    float minLuminance = 0.0f;
    float uiWhiteNits = 203.0f;
    // The scene's paper white in HDR (see DisplaySettings::paperWhiteNits).
    float paperWhiteNits = 250.0f;
    float exposureEv = 0.0f;
    float saturation = 1.0f;
    float sdrWhite = 1.0f;
    float sdrBlack = 0.0f;
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

// Whether to present HDR10. A scripted run (--frames, --state) on Auto stays SDR, so its captures
// do not depend on the desktop it ran on.
bool WantsHdrOutput(const DisplaySettings& settings, const DisplayReport& report, bool scriptedRun);

// The calibrated values, or what the display reports until there are some, held to their ranges;
// hdr is whether the swapchain is HDR10. A paper white that follows the UI white takes the resolved
// one; a caller that overrides uiWhiteNits afterwards calls FollowUiWhite again.
DisplayOutput ResolveDisplayOutput(const DisplaySettings& settings, const DisplayReport& report, bool hdr);

// SMPTE ST 2084: cd/m^2 to the PQ signal in [0, 1] and back. The calibration's levels step evenly
// in PQ, as the eye sees them.
float PqFromNits(float nits);
// Sets output.paperWhiteNits to output.uiWhiteNits when settings say it follows the UI white.
void FollowUiWhite(const DisplaySettings& settings, DisplayOutput& output);
// The scale HDR output applies to the scene before GT7's curve so its paper white lands at
// output.paperWhiteNits (1 in SDR).
float HdrPaperWhiteScale(const DisplayOutput& output);
float NitsFromPq(float pq);
}
