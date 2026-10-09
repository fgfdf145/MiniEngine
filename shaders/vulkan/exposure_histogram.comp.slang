#include "d3d_register.slang"
#include "shader_helpers.slang"
#include "exposure_histogram.slang"
#include "reverse_depth.slang"

// One invocation per pixel, and one per bin: the workgroup has exactly
// kExposureHistogramBinCount invocations, so each clears and flushes one shared bin.
// Read with Load only, so no samplers (VulkanExposureHistogramPass's NVRHI binding set).
[[vk::binding(0, 0)]] Texture2D hdrTexture : register(t0, space0);
[[vk::binding(1, 0)]] Texture2D depthTexture : register(t1, space0);

// Cleared by the pass before this dispatch and read back on the CPU once the frame's fence has
// signaled.
struct HistogramBuffer
{
    uint bins[kExposureHistogramBinCount];
    // The view's average colour for auto white balance: each metered pixel adds its rgb divided by
    // its luminance, in 1/kColorScale units, then the pixel count. Must match
    // VulkanExposureHistogramPass::GetFrameColor.
    uint colorSums[3];
    uint colorCount;
};
[[vk::binding(2, 0)]] RWStructuredBuffer<HistogramBuffer> histogram : register(u2, space0); // one element

static const float kColorScale = 1024.0;

struct HistogramConstants
{
    uint2 extent;
    // 1 when the background is a physical sky (the atmosphere or an HDRI), metered like geometry.
    uint meterBackground;
    // The HDR target is pre-exposed; this turns it back into physical luminance for the bins.
    float invPreExposure;
};
[[vk::push_constant]] ConstantBuffer<HistogramConstants> constants D3D_PUSH_CONSTANTS(0);

groupshared uint localBins[kExposureHistogramBinCount];
groupshared uint localColor[4];

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    localBins[groupIndex] = 0u;
    if (groupIndex < 4u)
    {
        localColor[groupIndex] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    int2 pixel = int2(dispatchThreadId.xy);
    if (all(dispatchThreadId.xy < constants.extent))
    {
        // The flat background of EnvironmentMode::None is skipped: it is a fixed display value, not
        // radiance, so metering it would feed the exposure back into itself. A physical sky is
        // radiance like any surface and is metered.
        // The depth is at the render size, which DLSS upscales from.
        int2 depthPixel = pixel * TextureSize(depthTexture, 0) / TextureSize(hdrTexture, 0);
        float depth = depthTexture.Load(int3(depthPixel, 0)).r;
        if (!IsFarDepth(depth) || constants.meterBackground != 0u)
        {
            float3 radiance = min(hdrTexture.Load(int3(pixel, 0)).rgb, float3(65504.0)) * constants.invPreExposure;
            float luminance = ExposureLuminance(radiance);
            // NaN fails every comparison; such a pixel is left out rather than binned arbitrarily.
            if (luminance >= 0.0)
            {
                InterlockedAdd(localBins[ExposureHistogramBin(luminance)], 1u);
            }
            // Chromaticity only, so a bright highlight weighs as much as a dim wall; capped so a
            // saturated pixel cannot overflow the sums.
            if (luminance > 1e-4)
            {
                uint3 color = uint3(min(radiance / luminance, float3(4.0)) * kColorScale);
                InterlockedAdd(localColor[0], color.r);
                InterlockedAdd(localColor[1], color.g);
                InterlockedAdd(localColor[2], color.b);
                InterlockedAdd(localColor[3], 1u);
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    uint count = localBins[groupIndex];
    if (count != 0u)
    {
        InterlockedAdd(histogram[0].bins[groupIndex], count);
    }
    if (groupIndex < 3u)
    {
        InterlockedAdd(histogram[0].colorSums[groupIndex], localColor[groupIndex]);
    }
    else if (groupIndex == 3u)
    {
        InterlockedAdd(histogram[0].colorCount, localColor[3]);
    }
}
