#version 450
#extension GL_GOOGLE_include_directive : require

#include "gt7_tonemap.glsl"

layout(set = 0, binding = 0) uniform sampler2D hdrTexture;

layout(push_constant) uniform TonemapConstants
{
    // Physical radiance to pre-exposed value, from the camera's EV100 (see ExposureFromEv100).
    float exposure;
}
constants;

layout(location = 0) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

void main()
{
    // Radiance is stored raw, so clamp below fp16's maximum before the operator: an infinite
    // input would turn into NaN inside it and show a very bright pixel as black.
    vec3 color = min(texture(hdrTexture, fragTexCoord).rgb, vec3(65504.0));

    // The HDR target holds radiance in physical units, where a sunlit surface is in the hundreds
    // or thousands, so the operator below only sees a usable range once the exposure is applied.
    color *= constants.exposure;

    // GT7's operator (see gt7_tonemap.glsl). The result is display-referred linear Rec.709; the
    // LDR target's sRGB format applies the transfer function on write.
    color = TonemapExposedRec709(color);

    // This pass is the sole writer of the LDR target and knows coverage is total, so it writes
    // alpha explicitly rather than relying on the RGB-only color write mask the material
    // pipelines use to keep the attachment's clear alpha intact. ImGui composites the viewport
    // image over the editor, so this alpha must be 1.0.
    outColor = vec4(color, 1.0);
}
