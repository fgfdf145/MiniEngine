// Depth only: renders a caster into one cascade of the directional shadow map (or the selection's own
// depth), passing the alpha test its texture coordinates. The C++ side of the push constant block is
// ShadowPushConstants in engine/renderer/vulkan/shadow_pass.h.
struct ShadowConstants
{
    float4x4 lightModelViewProjection;
    uint drawSlot;
};
#include "shader_helpers.slang"
#include "d3d_register.slang"

[[vk::push_constant]] ConstantBuffer<ShadowConstants> shadowData D3D_PUSH_CONSTANTS(2);

// Locations in the order the input layout lists them (ShadowInputLayout).
struct VertexInput
{
    [[vk::location(0)]] float3 inPosition : POSITION;
    [[vk::location(1)]] float2 inTexCoord : TEXCOORD;
    [[vk::location(2)]] float2 inTexCoord1 : SECOND_TEXCOORD;
};

struct VertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 fragTexCoord : TEXCOORD0;
    [[vk::location(1)]] float2 fragTexCoord1 : TEXCOORD1;
};

[shader("vertex")]
VertexOutput main(VertexInput input)
{
    VertexOutput output;
    output.position = ClipPosition(mul(shadowData.lightModelViewProjection, float4(input.inPosition, 1.0)));
    output.fragTexCoord = input.inTexCoord;
    output.fragTexCoord1 = input.inTexCoord1;
    return output;
}
