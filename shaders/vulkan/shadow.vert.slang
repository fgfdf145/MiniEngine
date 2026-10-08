// Depth only: renders a caster into one cascade of the directional shadow map. The C++ side of the
// push constant block is ShadowPushConstants in engine/renderer/vulkan/shadow_pass.h.
struct ShadowConstants
{
    float4x4 lightModelViewProjection;
    float4 baseColorFactor;
    float4 nodeGraphFactors;
    float4 alphaCutoffAndPadding; // x = alpha cutoff
    // The base colour's texture transform (material_uv.slang): (a, b, tx, UV set), (c, d, ty, 0).
    float4 baseColorTransformRow0;
    float4 baseColorTransformRow1;
};
[[vk::push_constant]] ConstantBuffer<ShadowConstants> shadowData;

struct VertexInput
{
    [[vk::location(0)]] float3 inPosition;
    [[vk::location(2)]] float2 inTexCoord;
    [[vk::location(5)]] float2 inTexCoord1;
};

struct VertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 fragTexCoord;
    [[vk::location(1)]] float2 fragTexCoord1;
};

[shader("vertex")]
VertexOutput main(VertexInput input)
{
    VertexOutput output;
    output.position = mul(shadowData.lightModelViewProjection, float4(input.inPosition, 1.0));
    output.fragTexCoord = input.inTexCoord;
    output.fragTexCoord1 = input.inTexCoord1;
    return output;
}
