#include "d3d_register.slang"
#include "scene_common.slang"
#include "atmosphere_sampling.slang"
#include "volumetric_clouds.slang"
#include "cubemap_common.slang"

// Must match the push constant VulkanForwardPass::RecordSky pushes.
struct SkyConstants
{
    // xyz = kViewportBackgroundFrameBuffer, the flat background of EnvironmentMode::None, already
    // in HDR target units: it stands for no physical light and is not pre-exposed. w = the roughness
    // whose prefiltered environment an HDRI background shows (the Khronos reference view's blur),
    // 0 for the map itself.
    float4 backgroundRadiance;
};
[[vk::push_constant]] ConstantBuffer<SkyConstants> skyData D3D_PUSH_CONSTANTS(1);

// Set 0 binding 28: the clouds at the scene's extent on the unjittered pixel grid (cloud_resolve.comp),
// rgb times CLOUD_TARGET_SCALE over the transmittance.
[[vk::binding(28, 0)]] Texture2D cloudTarget : register(t28, space0);

[shader("fragment")]
float4 main([[vk::location(0)]] float2 fragTexCoord : TEXCOORD0, float4 fragCoord : SV_Position) : SV_Target0
{
    uint mode = EnvironmentMode();
    if (mode == ENVIRONMENT_NONE)
    {
        return float4(skyData.backgroundRadiance.rgb, 1.0);
    }
    float3 direction = ViewDirectionFromTexCoord(fragTexCoord);
    float3 luminance = mode == ENVIRONMENT_HDRI ? SampleEnvironmentMap(direction) : SampleSky(direction);
    if (mode == ENVIRONMENT_HDRI && skyData.backgroundRadiance.w > 0.0)
    {
        luminance = prefilteredEnvironment.SampleLevel(prefilteredEnvironmentSampler, direction, skyData.backgroundRadiance.w * (PREFILTER_MIP_COUNT - 1.0)).rgb;
    }
    if (mode == ENVIRONMENT_ATMOSPHERE)
    {
        // The clouds hide the sky and the sun behind them and fade into the sky's own haze. They
        // are resolved per pixel on the unjittered grid and read as they are: a bilinear read at
        // the jittered position would spread every edge over a pixel, and their 15 m edges need no
        // further antialiasing.
        if (CloudsEnabled())
        {
            float4 clouds = cloudTarget.Load(int3(int2(fragCoord.xy), 0));
            luminance = luminance * clouds.a + clouds.rgb / CLOUD_TARGET_SCALE;
        }
        // Here and not in SampleSky, which also feeds the probe and the sky SH the fog is lit by. A
        // seamless horizon fogs the sky below it as the mirrored sky above: no ground to end the
        // fog's column, so no wall of fog at and below the horizon either.
        float3 fogDirection = SeamlessHorizon() ? float3(direction.x, abs(direction.y), direction.z) : direction;
        luminance = ApplyHeightFogToSky(luminance, fogDirection);
    }
    // Pre-exposed (see pre_exposure.slang). The sun disk alone is ~1e9 cd/m^2, which now fits in half
    // float at daylight exposure; the clamp stays for HDRI texels and low EVs, since the target must
    // never hold infinity.
    return float4(min(luminance * ubo.exposure.x, float3(65504.0)), 1.0);
}
