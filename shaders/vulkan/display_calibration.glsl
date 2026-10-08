// The display calibration's output adjustments and its patterns
// (docs/design/2026-10-08-display-calibration-design.md). Included by tonemap.frag after
// hdr_output.glsl, and by tests/tonemap_tests.cpp; written, like gt7_tonemap.glsl, in the subset
// GLSL and C++/GLM both accept (f-suffixed literals, no out parameters, no layouts).

// Must match CalibrationPattern in engine/renderer/display_calibration.h.
const uint CALIBRATION_NONE = 0u;
const uint CALIBRATION_HDR_FULL_FRAME = 1u;
const uint CALIBRATION_HDR_WINDOW = 2u;
const uint CALIBRATION_HDR_BLACK = 3u;
const uint CALIBRATION_SDR_BRIGHT = 4u;
const uint CALIBRATION_SDR_DARK = 5u;
const uint CALIBRATION_SAMPLE_WEDGE = 6u;
const uint CALIBRATION_SAMPLE_SKY = 7u;

// The ring the HDR patterns hide: at 10 000 cd/m^2 (clipped by every display) or at 0.
const float kCalibrationRingNits = 10000.0f;
// The checkerboards' second level, in sRGB signal: a step a good display just shows.
const float kCalibrationBrightCheck = 0.96f;
const float kCalibrationDarkCheck = 0.04f;

// SMPTE ST 2084 EOTF: the PQ signal in [0, 1] to cd/m^2 (PqEncodeNits undone).
float PqDecodeNits(float signal)
{
    float e = pow(clamp(signal, 0.0f, 1.0f), 1.0f / 78.84375f);
    return 10000.0f * pow(max(e - 0.8359375f, 0.0f) / (18.8515625f - 18.6875f * e), 1.0f / 0.1593017578125f);
}

// ITU-R BT.2390's black level lift, in PQ: E' = E + b (1 - E)^4 with b = PQ(floor). 0 cd/m^2 goes to
// the display's floor, so the darkest detail lands where the display still shows it; bright values
// barely move.
float LiftToBlackFloorNits(float nits, float floorNits)
{
    if (floorNits <= 0.0f)
    {
        return nits;
    }
    float e = PqEncodeNits(nits);
    float b = PqEncodeNits(floorNits);
    float inverse = 1.0f - e;
    return PqDecodeNits(e + b * inverse * inverse * inverse * inverse);
}

vec3 LiftToBlackFloorNits3(vec3 nits, float floorNits)
{
    return vec3(LiftToBlackFloorNits(nits.x, floorNits), LiftToBlackFloorNits(nits.y, floorNits), LiftToBlackFloorNits(nits.z, floorNits));
}

float SrgbEncode(float linear)
{
    float c = clamp(linear, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * pow(c, 1.0f / 2.4f) - 0.055f;
}

float SrgbDecode(float signal)
{
    float c = clamp(signal, 0.0f, 1.0f);
    return c <= 0.04045f ? c / 12.92f : pow((c + 0.055f) / 1.055f, 2.4f);
}

// SDR bright and dark section correction: the sRGB signal s goes to black + s (white - black), so a
// display that clips near white or crushes near black shows the detail again. Linear in and out
// (the SDR target encodes on write).
float CorrectSdrSignal(float linear, float white, float black)
{
    return SrgbDecode(black + SrgbEncode(linear) * (white - black));
}

vec3 CorrectSdr3(vec3 linear, float white, float black)
{
    return vec3(CorrectSdrSignal(linear.x, white, black), CorrectSdrSignal(linear.y, white, black), CorrectSdrSignal(linear.z, white, black));
}

// GT7's Saturation, before the curve: the colour's distance from its luminance (Rec.709) scaled.
vec3 AdjustSaturation(vec3 color, float saturation)
{
    float luminance = dot(color, vec3(0.2126f, 0.7152f, 0.0722f));
    return max(vec3(luminance) + (color - vec3(luminance)) * saturation, vec3(0.0f));
}

// Whether the ring covers the point; p is centred on the screen, in screen heights.
bool CalibrationRing(vec2 p)
{
    float radius = length(p);
    return abs(radius - 0.11f) < 0.018f;
}

// A checkerboard of 1/16-height cells over the middle half of the screen's height.
bool CalibrationCheck(vec2 p)
{
    if (abs(p.x) > 0.25f || abs(p.y) > 0.25f)
    {
        return false;
    }
    float cellX = floor((p.x + 0.25f) * 16.0f);
    float cellY = floor((p.y + 0.25f) * 16.0f);
    return mod(cellX + cellY, 2.0f) >= 1.0f;
}

// An HDR measurement pattern in cd/m^2 (grey). p is centred, in screen heights; aspect is width over
// height.
float CalibrationHdrNits(uint pattern, float level, vec2 p, float aspect)
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

// An SDR checkerboard as sRGB signal, before the correction: white near white, or black near black.
float CalibrationSdrSignal(uint pattern, vec2 p)
{
    bool check = CalibrationCheck(p);
    if (pattern == CALIBRATION_SDR_BRIGHT)
    {
        return check ? kCalibrationBrightCheck : 1.0f;
    }
    return check ? kCalibrationDarkCheck : 0.0f;
}

// The test cards, in GT frame-buffer units (2.5 is GT7's 250 cd/m^2 paper white), linear Rec.709.
// uv is [0, 1] from the top left.
vec3 CalibrationSampleWedge(vec2 uv)
{
    if (uv.y < 0.5f)
    {
        // Thirteen one-stop steps from 1/64 to 64 times paper white.
        float step = floor(clamp(uv.x, 0.0f, 0.9999f) * 13.0f);
        return vec3(2.5f * exp2(step - 6.0f));
    }
    // Six saturated patches at an eighth of paper white over six at four times it.
    float column = floor(clamp(uv.x, 0.0f, 0.9999f) * 6.0f);
    vec3 hue = vec3(1.0f, 0.05f, 0.05f);
    if (column == 1.0f)
    {
        hue = vec3(1.0f, 0.85f, 0.05f);
    }
    else if (column == 2.0f)
    {
        hue = vec3(0.05f, 1.0f, 0.05f);
    }
    else if (column == 3.0f)
    {
        hue = vec3(0.05f, 0.9f, 1.0f);
    }
    else if (column == 4.0f)
    {
        hue = vec3(0.05f, 0.05f, 1.0f);
    }
    else if (column == 5.0f)
    {
        hue = vec3(1.0f, 0.05f, 0.9f);
    }
    return hue * (uv.y < 0.75f ? 2.5f / 8.0f : 2.5f * 4.0f);
}

vec3 CalibrationSampleSky(vec2 uv, float aspect)
{
    vec2 sun = vec2(0.68f, 0.28f);
    vec2 d = (uv - sun) * vec2(aspect, 1.0f);
    float distance = length(d);
    vec3 color;
    if (uv.y < 0.62f)
    {
        // Deep blue overhead to a pale haze at the horizon.
        float t = uv.y / 0.62f;
        color = mix(vec3(0.35f, 0.75f, 2.2f), vec3(2.6f, 3.0f, 3.4f), t * t);
    }
    else
    {
        // Sunlit ground fading into shade toward the bottom.
        float t = (uv.y - 0.62f) / 0.38f;
        color = mix(vec3(1.1f, 0.95f, 0.75f), vec3(0.05f, 0.05f, 0.06f), t);
    }
    // The sun's disc, far brighter than any display (400 000 cd/m^2 at the frame buffer's scale),
    // and its glow.
    color += vec3(1.0f, 0.9f, 0.75f) * (6.0f / (1.0f + 4000.0f * distance * distance));
    if (distance < 0.02f)
    {
        color = vec3(4000.0f, 3800.0f, 3400.0f);
    }
    return color;
}
