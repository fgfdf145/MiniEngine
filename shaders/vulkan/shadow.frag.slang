// The alpha test for Mask materials in the shadow pass. Opaque casters are drawn with no fragment
// shader at all. The coverage here must match triangle.frag's, or cutouts would cast the wrong
// shape: the same two-layer base color blend, and the same cutoff against its alpha. The material
// comes from the frame set (set 0) at the caster's draw slot, its textures from the material set (set 1).
struct ShadowConstants
{
    float4x4 lightModelViewProjection;
    uint drawSlot;
};
#include "d3d_register.slang"
#include "shader_helpers.slang"
#include "scene_common.slang"
#include "material_common.slang"

[[vk::push_constant]] ConstantBuffer<ShadowConstants> shadowData D3D_PUSH_CONSTANTS(2);

[[vk::binding(0, 1)]] Texture2D baseColorTexture : register(t0, space1);
#define baseColorTextureSampler MaterialSampler(0)
[[vk::binding(6, 1)]] Texture2D secondaryBaseColorTexture : register(t6, space1);
#define secondaryBaseColorTextureSampler MaterialSampler(6)
[[vk::binding(12, 1)]] Texture2D blendMaskTexture : register(t12, space1);
#define blendMaskTextureSampler MaterialSampler(12)
// Each draw's texture transforms (material_uv.slang), 27 slots of two rows: the base colour's is slot 0,
// (a, b, tx, UV set) and (c, d, ty, 0).
[[vk::binding(17, 0)]] StructuredBuffer<float4> materialTextureTransforms : register(t17, space0);

[shader("fragment")]
void main([[vk::location(0)]] float2 fragTexCoord : TEXCOORD0, [[vk::location(1)]] float2 fragTexCoord1 : TEXCOORD1)
{
    MaterialData material = materials[shadowData.drawSlot];
    LoadMaterialSamplers(material.samplerIndices);
    float4 baseColorTransformRow0 = materialTextureTransforms[shadowData.drawSlot * 54u];
    float4 baseColorTransformRow1 = materialTextureTransforms[shadowData.drawSlot * 54u + 1u];
    // Layer B and the mask sample untransformed, as in the main passes.
    float3 uv = float3(baseColorTransformRow0.w > 0.5 ? fragTexCoord1 : fragTexCoord, 1.0);
    float2 baseColorUv = float2(dot(baseColorTransformRow0.xyz, uv), dot(baseColorTransformRow1.xyz, uv));
    float blendMask = blendMaskTexture.Sample(blendMaskTextureSampler, fragTexCoord).r;
    float blendWeight = clamp(
        lerp(0.0, material.nodeGraphFactors.y, clamp(material.nodeGraphFactors.x, 0.0, 1.0)) * blendMask,
        0.0, 1.0);
    float alpha = lerp(
                      baseColorTexture.Sample(baseColorTextureSampler, baseColorUv).a,
                      secondaryBaseColorTexture.Sample(secondaryBaseColorTextureSampler, fragTexCoord).a,
                      blendWeight) *
                  material.baseColorFactor.a;
    if (alpha < material.alphaCutoff)
        discard;
}
