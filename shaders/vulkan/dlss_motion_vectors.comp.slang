// The motion vectors DLSS reads (VulkanTaaPass, docs/design/2026-10-07-dlss-design.md): for every
// render pixel, where its content was last frame relative to where it is now, in render pixels, x
// right and y down, without the jitter. The geometry pass writes the opposite (current minus
// previous, in UV) for the surfaces it draws; the background writes nothing, so it moves with the
// camera alone and is reprojected here, as the TAA resolve does.

#include "scene_common.slang"

// Set 1 is VulkanTaaPass's NVRHI binding set; both inputs are read with Load.
[[vk::binding(0, 1)]] Texture2D depthTexture;
[[vk::binding(1, 1)]] Texture2D velocityTexture;
[[vk::binding(2, 1)]] [[vk::image_format("rg16f")]] RWTexture2D<float2> motionImage;

// Must match DlssMotionPushConstants in engine/renderer/vulkan/taa_pass.cpp.
struct DlssMotionConstants
{
    float2 extent;
    float2 invExtent;
    // This frame's jitter in render pixels: the image is shifted by it, so the pixel's unjittered
    // position is its centre minus the jitter.
    float2 jitterPixels;
    float2 unused;
};
[[vk::push_constant]] ConstantBuffer<DlssMotionConstants> motion;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 pixel = int2(dispatchThreadId.xy);
    if (any(pixel >= int2(motion.extent)))
    {
        return;
    }

    float2 uv = (float2(pixel) + 0.5) * motion.invExtent;
    float2 velocity;
    if (IsFarDepth(depthTexture.Load(int3(pixel, 0)).r))
    {
        // invViewProj carries the jitter, so the direction it gives is the one this pixel saw; last
        // frame's view-projection does not, and neither does the pixel's unjittered position.
        float4 world = mul(ubo.invViewProj, float4(uv * 2.0 - 1.0, DEPTH_FAR, 1.0));
        float4 previousClip = mul(ubo.prevViewProj, float4(world.xyz / world.w, 1.0));
        float2 previousUv = previousClip.xy / previousClip.w * 0.5 + 0.5;
        velocity = (uv - motion.jitterPixels * motion.invExtent) - previousUv;
    }
    else
    {
        velocity = velocityTexture.Load(int3(pixel, 0)).rg;
    }
    motionImage[pixel] = -velocity * motion.extent;
}
