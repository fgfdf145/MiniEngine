// Depth only, for opaque casters: shadow.vert without the UVs the alpha test needs, reading the
// position-only stream (VulkanBuffer::GetPositionHandle). The block is shadow.vert's; only its first
// member is read.
struct ShadowConstants
{
    float4x4 lightModelViewProjection;
};
[[vk::push_constant]] ConstantBuffer<ShadowConstants> shadowData;

[shader("vertex")]
float4 main([[vk::location(0)]] float3 inPosition : POSITION) : SV_Position
{
    return mul(shadowData.lightModelViewProjection, float4(inPosition, 1.0));
}
