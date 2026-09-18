// The luminance histogram's binning rule, shared by exposure_histogram.comp, which fills the
// bins, and engine/renderer/exposure.cpp, which meters from them. Like gt7_tonemap.glsl it is also
// compiled as C++ (inside a namespace with `using namespace glm` and `using uint = unsigned int`),
// so it keeps to the subset both languages accept: f-suffixed float literals, u-suffixed unsigned
// literals, no #version, layouts or includes.

// 256 bins, one per invocation of the 16 x 16 workgroup that clears and flushes them.
const uint kExposureHistogramBinCount = 256u;

// log2 of the luminance range the bins span, in cd/m^2: about 0.00024 to 262144, 30 stops at
// roughly 0.12 stop per bin. Darker pixels, black included, land in the first bin and brighter
// ones in the last, so they still count toward the average instead of vanishing from it.
const float kExposureHistogramMinLog2Luminance = -12.0f;
const float kExposureHistogramMaxLog2Luminance = 18.0f;

// Photometric luminance of linear Rec.709 radiance.
float ExposureLuminance(vec3 radiance)
{
    return dot(radiance, vec3(0.2126f, 0.7152f, 0.0722f));
}

float ExposureHistogramBinWidthLog2()
{
    return (kExposureHistogramMaxLog2Luminance - kExposureHistogramMinLog2Luminance) /
           float(kExposureHistogramBinCount);
}

uint ExposureHistogramBin(float luminance)
{
    // The floor keeps log2 finite for black.
    float log2Luminance = log2(max(luminance, 1e-12f));
    float position = (log2Luminance - kExposureHistogramMinLog2Luminance) / ExposureHistogramBinWidthLog2();
    return uint(clamp(position, 0.0f, float(kExposureHistogramBinCount - 1u)));
}

float ExposureHistogramBinCenterLog2(uint bin)
{
    return kExposureHistogramMinLog2Luminance + (float(bin) + 0.5f) * ExposureHistogramBinWidthLog2();
}
