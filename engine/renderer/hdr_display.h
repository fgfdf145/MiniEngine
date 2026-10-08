#pragma once

// The two luminances HDR output is built on (docs/design/2026-10-08-viewport-only-hdr-design.md),
// shared by the renderer and the Graphics Debug window's report.

#include <engine/platform/display/display_hdr.h>
#include <engine/renderer/render_types.h>

#include <algorithm>

namespace me
{

// Where SDR white shows under HDR output, in cd/m^2: Windows' SDR content brightness, where the
// editor shows in SDR, so the UI and the scene's paper white land there. ITU-R BT.2408's graphics
// white where the platform reports nothing.
inline float HdrSdrWhiteNits(const platform::display::DisplayHdrInfo& display)
{
    return display.known && display.sdrWhiteNits > 0.0f ? display.sdrWhiteNits : 203.0f;
}

// The peak GT7's HDR curve is built for: the display's reported one, or the setting when that is off
// or there is none; never below SDR white (the curve then only rolls off what SDR would clip).
inline float HdrPeakNits(const RenderDebugSettings& settings, const platform::display::DisplayHdrInfo& display)
{
    const bool reported = settings.hdrPeakFromDisplay && display.known && display.maxLuminance > 0.0f;
    const float peak = reported ? display.maxLuminance : settings.hdrPeakNits;
    return std::clamp(std::max(peak, HdrSdrWhiteNits(display)), 250.0f, 10000.0f);
}
}
