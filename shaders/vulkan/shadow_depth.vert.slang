// Depth only, no alpha test: renders an opaque caster from its position-only stream. The push
// constant block is ShadowPushConstants in engine/renderer/vulkan/shadow_pass.h.
struct ShadowConstants
{
    float4x4 lightModelViewProjection;
    uint drawSlot;
};
#include "d3d_register.slang"
#include "shader_helpers.slang"

[[vk::push_constant]] ConstantBuffer<ShadowConstants> shadowData D3D_PUSH_CONSTANTS(2);

[shader("vertex")]
float4 main([[vk::location(0)]] float3 inPosition : POSITION) : SV_Position
{
    return ClipPosition(mul(shadowData.lightModelViewProjection, float4(inPosition, 1.0)));
}
