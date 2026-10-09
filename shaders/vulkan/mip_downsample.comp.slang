// One mip level from the one above it, each layer of an array (a cube's six faces): every target
// texel is the source bilinearly sampled at its centre, which is what a linear blit of the level
// gave (the 2x2 average where the source size is even). VulkanMipDownsample dispatches it; NVRHI
// and D3D12 have no blit.
#include "d3d_register.slang"

struct DownsampleConstants
{
    // The target level's size in texels.
    uint2 targetSize;
    uint2 padding;
};

[[vk::push_constant]] ConstantBuffer<DownsampleConstants> constants D3D_PUSH_CONSTANTS(0);

// The source level alone (its view has one mip), and the target level: array views, or plain 2D
// ones for a 2D texture (MIP_DOWNSAMPLE_2D), whose view cannot be an array's.
#ifdef MIP_DOWNSAMPLE_2D
[[vk::binding(0, 0)]] Texture2D<float4> source : register(t0, space0);
[[vk::binding(1, 0)]] RWTexture2D<float4> target : register(u1, space0);
#else
[[vk::binding(0, 0)]] Texture2DArray<float4> source : register(t0, space0);
[[vk::binding(1, 0)]] RWTexture2DArray<float4> target : register(u1, space0);
#endif
[[vk::binding(64, 0)]] SamplerState linearClamp : register(s64, space0);

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= constants.targetSize))
    {
        return;
    }
    const float2 uv = (float2(id.xy) + 0.5) / float2(constants.targetSize);
#ifdef MIP_DOWNSAMPLE_2D
    target[id.xy] = source.SampleLevel(linearClamp, uv, 0.0);
#else
    target[id] = source.SampleLevel(linearClamp, float3(uv, float(id.z)), 0.0);
#endif
}
