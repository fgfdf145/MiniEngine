// The transmission copy's base level: the HDR target, everything opaque and the sky drawn, resampled
// bilinearly into the fixed-size copy (VulkanTransmissionCopyPass), which then builds its mipmaps.
// Pre-exposed values, as the HDR target holds them (pre_exposure.slang).

// Set 1 is VulkanTransmissionCopyPass's NVRHI binding set: the texture's sampler at 64.
[[vk::binding(0, 1)]] Texture2D sceneHdr;
[[vk::binding(64, 1)]] SamplerState sceneHdrSampler;
[[vk::binding(1, 1)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> copyLevel0;

// Must match TransmissionCopyConstants in engine/renderer/vulkan/transmission_copy.cpp.
struct TransmissionCopyConstants
{
    uint2 extent;
};
[[vk::push_constant]] ConstantBuffer<TransmissionCopyConstants> constants;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= constants.extent))
    {
        return;
    }
    float2 uv = (float2(pixel) + 0.5) / float2(constants.extent);
    // An infinite input would spread into every mip; the target never holds more than fp16's range.
    float3 color = min(sceneHdr.SampleLevel(sceneHdrSampler, uv, 0.0).rgb, float3(65504.0));
    copyLevel0[int2(pixel)] = float4(color, 1.0);
}
