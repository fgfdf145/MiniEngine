// The HDR calibration's patterns (docs/design/2026-10-10-hdr-calibration-design.md), in absolute
// cd/m^2. Included by tonemap.frag after hdr_output.slang, and by tests/display_calibration_tests.cpp;
// written, like gt7_tonemap.slang, in the subset Slang and C++/GLM both accept.

// Must match CalibrationPattern in engine/renderer/display_calibration.h.
static const uint CALIBRATION_NONE = 0u;
static const uint CALIBRATION_HDR_FULL_FRAME = 1u;
static const uint CALIBRATION_HDR_WINDOW = 2u;
static const uint CALIBRATION_HDR_BLACK = 3u;

// The ring the patterns hide: at 10 000 cd/m^2 (clipped by every display) or at 0.
static const float kCalibrationRingNits = 10000.0f;

// Whether the ring covers the point; p is centred on the screen, in screen heights.
bool CalibrationRing(float2 p)
{
    float radius = length(p);
    return abs(radius - 0.11f) < 0.018f;
}

// A pattern in cd/m^2 (grey). p is centred, in screen heights; aspect is width over height.
float CalibrationHdrNits(uint pattern, float level, float2 p, float aspect)
{
    if (pattern == CALIBRATION_HDR_FULL_FRAME)
    {
        return CalibrationRing(p) ? kCalibrationRingNits : level;
    }
    if (pattern == CALIBRATION_HDR_WINDOW)
    {
        // A square of 10 % of the screen's area.
        float halfSide = 0.5f * sqrt(0.1f * aspect);
        if (abs(p.x) > halfSide || abs(p.y) > halfSide)
        {
            return 0.0f;
        }
        return CalibrationRing(p) ? kCalibrationRingNits : level;
    }
    // CALIBRATION_HDR_BLACK
    return CalibrationRing(p) ? 0.0f : level;
}
