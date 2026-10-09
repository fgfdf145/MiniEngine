#include "display_calibration.h"

#include <algorithm>
#include <cmath>

namespace me
{

namespace
{
constexpr float kPqM1 = 0.1593017578125f;
constexpr float kPqM2 = 78.84375f;
constexpr float kPqC1 = 0.8359375f;
constexpr float kPqC2 = 18.8515625f;
constexpr float kPqC3 = 18.6875f;
}

DisplayOutput ResolveDisplayOutput(const DisplaySettings& settings, const DisplayReport& report, bool hdr)
{
    DisplayOutput output;
    output.hdr = hdr;
    if (settings.calibrated)
    {
        output.maxLuminance = settings.maxLuminance;
        output.maxFullFrameLuminance = settings.maxFullFrameLuminance;
        output.minLuminance = settings.minLuminance;
    }
    else if (report.known && report.maxLuminance > 0.0f)
    {
        output.maxLuminance = report.maxLuminance;
        output.maxFullFrameLuminance = report.maxFullFrameLuminance > 0.0f ? report.maxFullFrameLuminance : report.maxLuminance;
        output.minLuminance = report.minLuminance;
    }
    output.maxLuminance = std::clamp(output.maxLuminance, kMinCalibrationPeakNits, kMaxCalibrationPeakNits);
    output.maxFullFrameLuminance = std::clamp(output.maxFullFrameLuminance, kMinCalibrationPeakNits, kMaxCalibrationPeakNits);
    output.minLuminance = std::clamp(output.minLuminance, 0.0f, kMaxCalibrationBlackNits);

    if (settings.uiWhiteNits > 0.0f)
    {
        output.uiWhiteNits = settings.uiWhiteNits;
    }
    else if (report.known)
    {
        output.uiWhiteNits = report.sdrWhiteNits;
    }
    output.uiWhiteNits = std::clamp(output.uiWhiteNits, kMinUiWhiteNits, kMaxUiWhiteNits);
    output.paperWhiteNits = settings.paperWhiteNits > 0.0f ? std::clamp(settings.paperWhiteNits, kMinUiWhiteNits, kMaxUiWhiteNits) : output.uiWhiteNits;
    return output;
}

float HdrPaperWhiteScale(const DisplayOutput& output)
{
    return output.hdr ? output.paperWhiteNits / kGt7PaperWhiteNits : 1.0f;
}

float PqFromNits(float nits)
{
    const float y = std::clamp(nits / 10000.0f, 0.0f, 1.0f);
    const float ym = std::pow(y, kPqM1);
    return std::pow((kPqC1 + kPqC2 * ym) / (1.0f + kPqC3 * ym), kPqM2);
}

float NitsFromPq(float pq)
{
    const float e = std::pow(std::clamp(pq, 0.0f, 1.0f), 1.0f / kPqM2);
    const float y = std::pow(std::max(e - kPqC1, 0.0f) / (kPqC2 - kPqC3 * e), 1.0f / kPqM1);
    return y * 10000.0f;
}
}
