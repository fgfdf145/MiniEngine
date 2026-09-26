// Khronos PBR Neutral tone mapping (KhronosGroup/ToneMapping, PBR_Neutral), the Khronos glTF Sample
// Viewer's default operator. The engine uses it only in the Khronos reference view, to compare
// against the Sample Viewer on equal terms; GT7's operator (gt7_tonemap.glsl) stays the look.
//
// Compiled twice, like gt7_tonemap.glsl: by glslc from tonemap.frag and by
// tests/khronos_reference_tests.cpp inside a namespace with `using namespace glm`, so it keeps to
// the subset both accept (f-suffixed literals, no out parameters).
//
// Input is exposed linear Rec.709 (1.0 = the viewer's exposure 1.0); output is display linear.
// Below the start of compression colours pass through, less a small offset that lifts the toe;
// above it the peak channel rolls off towards 1 and the colour desaturates towards white.
const float kPbrNeutralStartCompression = 0.76f; // 0.8 - 0.04
const float kPbrNeutralDesaturation = 0.15f;

vec3 KhronosPbrNeutral(vec3 color)
{
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    color -= vec3(offset);

    float peak = max(color.r, max(color.g, color.b));
    if (peak < kPbrNeutralStartCompression)
    {
        return color;
    }

    float d = 1.0f - kPbrNeutralStartCompression;
    float newPeak = 1.0f - d * d / (peak + d - kPbrNeutralStartCompression);
    color *= newPeak / peak;

    float g = 1.0f - 1.0f / (kPbrNeutralDesaturation * (peak - newPeak) + 1.0f);
    return mix(color, vec3(newPeak), g);
}

// The Sample Viewer encodes its display-linear output with a pure 2.2 gamma (tonemapping.glsl,
// linearTosRGB), not the sRGB curve. The LDR target applies the sRGB curve on write, so this returns
// the value that curve turns into the viewer's encoding. Near black the two differ most: 0.0004 is
// about 7 of 255 in the viewer and 1 of 255 through sRGB, which made dark reflections look much
// darker in the engine than in the viewer (IORTestGrid's black spheres).
float KhronosViewerChannelForSrgbTarget(float displayLinear)
{
    float encoded = pow(max(displayLinear, 0.0f), 1.0f / 2.2f);
    return encoded <= 0.04045f ? encoded / 12.92f : pow((encoded + 0.055f) / 1.055f, 2.4f);
}

vec3 KhronosViewerOutputForSrgbTarget(vec3 displayLinear)
{
    return vec3(
        KhronosViewerChannelForSrgbTarget(displayLinear.r),
        KhronosViewerChannelForSrgbTarget(displayLinear.g),
        KhronosViewerChannelForSrgbTarget(displayLinear.b));
}
