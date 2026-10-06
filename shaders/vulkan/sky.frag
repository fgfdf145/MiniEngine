#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"
#include "atmosphere_sampling.glsl"
#include "volumetric_clouds.glsl"
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

// Set 0 binding 28: the clouds at the scene's extent on the unjittered pixel grid (cloud_resolve.comp),
// rgb times CLOUD_TARGET_SCALE over the transmittance.
layout(set = 0, binding = 28) uniform sampler2D cloudTarget;

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
    if (mode == ENVIRONMENT_ATMOSPHERE)
    {
        // The clouds hide the sky and the sun behind them and fade into the sky's own haze. They
        // are resolved per pixel on the unjittered grid and read as they are: a bilinear read at
        // the jittered position would spread every edge over a pixel, and their 15 m edges need no
        // further antialiasing.
        if (CloudsEnabled())
        {
            vec4 clouds = texelFetch(cloudTarget, ivec2(gl_FragCoord.xy), 0);
            luminance = luminance * clouds.a + clouds.rgb / CLOUD_TARGET_SCALE;
        }
        // Here and not in SampleSky, which also feeds the probe and the sky SH the fog is lit by. A
        // seamless horizon fogs the sky below it as the mirrored sky above: no ground to end the
        // fog's column, so no wall of fog at and below the horizon either.
        vec3 fogDirection = SeamlessHorizon() ? vec3(direction.x, abs(direction.y), direction.z) : direction;
        luminance = ApplyHeightFogToSky(luminance, fogDirection);
    }
    // Pre-exposed (see pre_exposure.glsl). The sun disk alone is ~1e9 cd/m^2, which now fits in half
    // float at daylight exposure; the clamp stays for HDRI texels and low EVs, since the target must
    // never hold infinity.
    outColor = vec4(min(luminance * ubo.exposure.x, vec3(65504.0)), 1.0);
}
