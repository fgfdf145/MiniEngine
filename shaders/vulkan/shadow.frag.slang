// The alpha test for Mask materials in the shadow pass. Opaque casters are drawn with no fragment
// shader at all. The coverage here must match triangle.frag's, or cutouts would cast the wrong
// shape: the same two-layer base color blend, and the same cutoff against its alpha.
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
#include "d3d_register.slang"

[[vk::push_constant]] ConstantBuffer<ShadowConstants> shadowData D3D_PUSH_CONSTANTS(0);

// The material set, bound at set 0 here: this pass has no camera set.
[[vk::binding(0, 0)]] Texture2D baseColorTexture : register(t0, space0);
[[vk::binding(64, 0)]] SamplerState baseColorTextureSampler : register(s64, space0);
[[vk::binding(6, 0)]] Texture2D secondaryBaseColorTexture : register(t6, space0);
[[vk::binding(70, 0)]] SamplerState secondaryBaseColorTextureSampler : register(s70, space0);
[[vk::binding(12, 0)]] Texture2D blendMaskTexture : register(t12, space0);
[[vk::binding(76, 0)]] SamplerState blendMaskTextureSampler : register(s76, space0);

[shader("fragment")]
void main([[vk::location(0)]] float2 fragTexCoord : TEXCOORD0, [[vk::location(1)]] float2 fragTexCoord1 : TEXCOORD1)
{
    // Layer B and the mask sample untransformed, as in the main passes.
    float3 uv = float3(shadowData.baseColorTransformRow0.w > 0.5 ? fragTexCoord1 : fragTexCoord, 1.0);
    float2 baseColorUv = float2(dot(shadowData.baseColorTransformRow0.xyz, uv), dot(shadowData.baseColorTransformRow1.xyz, uv));
    float blendMask = blendMaskTexture.Sample(blendMaskTextureSampler, fragTexCoord).r;
    float blendWeight = clamp(
        lerp(0.0, shadowData.nodeGraphFactors.y, clamp(shadowData.nodeGraphFactors.x, 0.0, 1.0)) * blendMask,
        0.0, 1.0);
    float alpha = lerp(
                      baseColorTexture.Sample(baseColorTextureSampler, baseColorUv).a,
                      secondaryBaseColorTexture.Sample(secondaryBaseColorTextureSampler, fragTexCoord).a,
                      blendWeight) *
                  shadowData.baseColorFactor.a;
    if (alpha < shadowData.alphaCutoffAndPadding.x)
        discard;
}
