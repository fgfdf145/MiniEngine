#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"
#include "atmosphere_sampling.glsl"
#include "cubemap_common.glsl"

// Must match the push constant VulkanForwardPass::RecordSky pushes.
layout(push_constant) uniform SkyConstants
{
    // xyz = kViewportBackgroundFrameBuffer, the flat background of EnvironmentMode::None, already
    // in HDR target units: it stands for no physical light and is not pre-exposed. w = the roughness
    // whose prefiltered environment an HDRI background shows (the Khronos reference view's blur),
    // 0 for the map itself.
    vec4 backgroundRadiance;
}
skyData;

layout(location = 0) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

void main()
{
    uint mode = EnvironmentMode();
    if (mode == ENVIRONMENT_NONE)
    {
        outColor = vec4(skyData.backgroundRadiance.rgb, 1.0);
        return;
    }
    vec3 direction = ViewDirectionFromTexCoord(fragTexCoord);
    vec3 luminance = mode == ENVIRONMENT_HDRI ? SampleEnvironmentMap(direction) : SampleSky(direction);
    if (mode == ENVIRONMENT_HDRI && skyData.backgroundRadiance.w > 0.0)
    {
        luminance = textureLod(prefilteredEnvironment, direction, skyData.backgroundRadiance.w * (PREFILTER_MIP_COUNT - 1.0)).rgb;
    }
    // Pre-exposed (see pre_exposure.glsl). The sun disk alone is ~1e9 cd/m^2, which now fits in half
    // float at daylight exposure; the clamp stays for HDRI texels and low EVs, since the target must
    // never hold infinity.
    outColor = vec4(min(luminance * ubo.exposure.x, vec3(65504.0)), 1.0);
}
