#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"
#include "atmosphere_sampling.glsl"

// Must match the push constant VulkanForwardPass::RecordSky pushes.
layout(push_constant) uniform SkyConstants
{
    // xyz = GetBackgroundRadiance(exposure), the flat background of EnvironmentMode::None.
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
    // The sun disk alone is ~1e9 cd/m^2, past half float; the target must never hold infinity.
    outColor = vec4(min(luminance, vec3(65504.0)), 1.0);
}
