#include "display_calibration.h"

#include <engine/renderer/shader_cpp_compat.h>

#include <algorithm>

// The PQ curve the shaders use, compiled as C++ (hdr_output.slang keeps to the subset both accept).
namespace me::hdr_output_shader
{
using namespace glm;
using namespace me::shader_cpp;
#include <shaders/vulkan/hdr_output.slang>
}

namespace me
{

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

float HdrBlackFloorPq(const DisplayOutput& output, CalibrationPattern pattern)
{
    if (!output.hdr || output.minLuminance <= 0.0f || pattern != CalibrationPattern::None)
    {
        return 0.0f;
    }
    return PqFromNits(output.minLuminance);
}

float PqFromNits(float nits)
{
    return hdr_output_shader::PqEncodeNits(nits);
}

float NitsFromPq(float pq)
{
    return hdr_output_shader::PqDecodeNits(pq);
}
}
