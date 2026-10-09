#include "d3d_register.slang"
#include "shader_helpers.slang"
#include "draw_parameters.slang"
#include "scene_common.slang"

// ObjectPushConstants in engine/renderer/material.h. The material is in set 0 binding 12.
struct DrawConstants
{
    float4x4 model;
};
[[vk::push_constant]] ConstantBuffer<DrawConstants> drawData D3D_PUSH_CONSTANTS(1);

// Each draw's model matrix from last frame, indexed by the draw's firstInstance (see
// VulkanDrawItem::motionSlot). Push constants are full, so it cannot ride with the current one.
[[vk::binding(2, 0)]] StructuredBuffer<float4x4> previousModels : register(t2, space0);

struct VertexInput
{
    [[vk::location(0)]] float3 inPosition : POSITION;
    [[vk::location(1)]] float3 inColor : COLOR;
    [[vk::location(2)]] float2 inTexCoord : TEXCOORD;
    [[vk::location(3)]] float3 inNormal : NORMAL;
    [[vk::location(4)]] float4 inTangent : TANGENT;
    [[vk::location(5)]] float2 inTexCoord1 : SECOND_TEXCOORD;
    // Where this vertex was last frame in the model's space: a skinned mesh's last pose, anyone else's
    // position (VulkanBuffer::GetPreviousPositionHandle).
    [[vk::location(7)]] float3 inPreviousPosition : PREVIOUS_POSITION;
    DRAW_INSTANCE_INDEX_FIELDS;
};

struct VertexOutput
{
    // The toon pass (toon.vert) draws the anime characters' opaque surfaces again on the depth this
    // writes for them in the geometry pass, from the same expression: invariant in both, so the two
    // agree to the bit. Slang cannot say so; the build marks it (MINIENGINE_INVARIANT_POSITION_SHADERS).
    float4 position : SV_Position;
    [[vk::location(0)]] float3 fragColor : TEXCOORD0;
    [[vk::location(1)]] float2 fragTexCoord : TEXCOORD1;
    [[vk::location(2)]] float3 fragWorldNormal : TEXCOORD2;
    [[vk::location(3)]] float4 fragWorldTangent : TEXCOORD3;
    [[vk::location(4)]] float3 fragWorldPosition : TEXCOORD4;
    // Clip positions of this vertex this frame and last frame, divided per fragment by gbuffer.frag
    // for motion vectors. triangle.frag does not declare them.
    [[vk::location(5)]] float4 fragCurrClip : TEXCOORD5;
    [[vk::location(6)]] float4 fragPrevClip : TEXCOORD6;
    // The draw's slot (its firstInstance), which the fragment shaders index the material buffer with.
    [[vk::location(7)]] nointerpolation uint fragDrawSlot : TEXCOORD7;
    [[vk::location(8)]] float2 fragTexCoord1 : TEXCOORD8;
    // The model matrix's per-axis scale, for the volume's thickness (KHR_materials_volume), which is in
    // mesh units.
    [[vk::location(9)]] nointerpolation float3 fragModelScale : TEXCOORD9;
    // The vertex in the model's space, which detail layers mapped by position tile by (detail_layers.slang).
    [[vk::location(10)]] float3 fragObjectPosition : TEXCOORD10;
};

[shader("vertex")]
VertexOutput main(VertexInput input)
{
    VertexOutput output;
    float4 worldPosition = mul(drawData.model, float4(input.inPosition, 1.0));

    // Compute the normal matrix analytically from the TRS model columns.
    // For M = R*S, the normal matrix (transpose of inverse) equals R*S^{-1},
    // which is each column divided by its squared length. This avoids inverse().
    float3 mc0 = ColumnOf(drawData.model, 0).xyz;
    float3 mc1 = ColumnOf(drawData.model, 1).xyz;
    float3 mc2 = ColumnOf(drawData.model, 2).xyz;
    float3x3 normalMatrix = MatrixFromColumns(
        mc0 / max(dot(mc0, mc0), 1e-6),
        mc1 / max(dot(mc1, mc1), 1e-6),
        mc2 / max(dot(mc2, mc2), 1e-6));

    float3 worldTangent = normalize(mul((float3x3)drawData.model, input.inTangent.xyz));

    output.position = mul(mul(ubo.proj, ubo.view), worldPosition);
    // Unjittered, like prevViewProj, so motion vectors measure motion and not the TAA jitter.
    output.fragCurrClip = mul(ubo.viewProjNoJitter, worldPosition);
    output.fragPrevClip = mul(ubo.prevViewProj, mul(previousModels[DRAW_INSTANCE_INDEX(input)], float4(input.inPreviousPosition, 1.0)));
    output.fragColor = input.inColor;
    output.fragTexCoord = input.inTexCoord;
    output.fragTexCoord1 = input.inTexCoord1;
    output.fragWorldNormal = normalize(mul(normalMatrix, input.inNormal));
    output.fragWorldTangent = float4(worldTangent, input.inTangent.w);
    output.fragWorldPosition = worldPosition.xyz;
    output.fragDrawSlot = uint(DRAW_INSTANCE_INDEX(input));
    output.fragObjectPosition = input.inPosition;
    output.fragModelScale = float3(length(ColumnOf(drawData.model, 0).xyz), length(ColumnOf(drawData.model, 1).xyz), length(ColumnOf(drawData.model, 2).xyz));
    return output;
}
