// One submesh's ray material (RayMaterial in ray_tracing_common.slang): what a DDGI probe ray sees of
// it, without textures. A 16 x 16 grid of samples over the texture at the mip nearest 16 texels
// across: the albedo is the base colour's average over the covered samples, the coverage the share
// of samples that stop light (Mask: alpha at or above the cutoff; Blend: the alpha itself, though
// the ray scene leaves Blend surfaces out of the traces; Opaque: all), times what transmission lets
// through; the emission the emissive texture's average. Texture
// transforms and the second UV set are ignored: an average does not depend on them much.

#include "d3d_register.slang"
#include "shader_helpers.slang"

[[vk::binding(0, 0)]] Texture2D baseColorTexture : register(t0, space0);
[[vk::binding(64, 0)]] SamplerState baseColorTextureSampler : register(s64, space0);
[[vk::binding(1, 0)]] Texture2D emissiveTexture : register(t1, space0);
[[vk::binding(65, 0)]] SamplerState emissiveTextureSampler : register(s65, space0);

struct RayMaterial
{
    float4 albedoCoverage;
    float4 emissionFlags;
    uint4 samplers;
};

// Every slot's set names its textures; the materials buffer is one set for all of them.
[[vk::binding(0, 1)]] RWStructuredBuffer<RayMaterial> rayMaterials : register(u0, space1);

// Must match RayMaterialConstants in engine/renderer/vulkan/ray_scene.cpp.
struct RayMaterialConstants
{
    float4 baseColorFactor;
    float4 emissiveAndCutoff; // rgb emissive factor, a alpha cutoff
    uint4 params;           // x output index, y alpha mode, z flags, w transmission as float bits
    uint4 samplers;         // RayMaterial's samplers, passed through
};
[[vk::push_constant]] ConstantBuffer<RayMaterialConstants> constants D3D_PUSH_CONSTANTS(0);

static const uint ALPHA_OPAQUE = 0u;
static const uint ALPHA_MASK = 1u;

groupshared float4 sharedAlbedo[256];   // rgb covered albedo sum, a covered count
groupshared float4 sharedEmission[256]; // rgb emission sum, a coverage sum

float LodForSize(Texture2D image, float texels)
{
    float levels = float(TextureLevels(image));
    float width = float(TextureSize(image, 0).x);
    return clamp(log2(max(width / texels, 1.0)), 0.0, levels - 1.0);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 groupThreadId : SV_GroupThreadID, uint groupIndex : SV_GroupIndex)
{
    uint index = groupIndex;
    float2 uv = (float2(groupThreadId.xy) + 0.5) / 16.0;
    float4 base = baseColorTexture.SampleLevel(baseColorTextureSampler, uv, LodForSize(baseColorTexture, 16.0)) * constants.baseColorFactor;
    float3 emission = emissiveTexture.SampleLevel(emissiveTextureSampler, uv, LodForSize(emissiveTexture, 16.0)).rgb * constants.emissiveAndCutoff.rgb;

    float coverage = 1.0;
    if (constants.params.y == ALPHA_MASK)
    {
        coverage = base.a >= constants.emissiveAndCutoff.a ? 1.0 : 0.0;
    }
    else if (constants.params.y != ALPHA_OPAQUE)
    {
        coverage = clamp(base.a, 0.0, 1.0);
    }
    sharedAlbedo[index] = float4(base.rgb * coverage, coverage);
    sharedEmission[index] = float4(emission, coverage);
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = 128u; stride > 0u; stride >>= 1u)
    {
        if (index < stride)
        {
            sharedAlbedo[index] += sharedAlbedo[index + stride];
            sharedEmission[index] += sharedEmission[index + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (index == 0u)
    {
        float4 albedoSum = sharedAlbedo[0];
        float3 albedo = albedoSum.a > 0.0 ? albedoSum.rgb / albedoSum.a : constants.baseColorFactor.rgb;
        float transmission = asfloat(constants.params.w);
        float meanCoverage = sharedEmission[0].a / 256.0 * (1.0 - transmission);
        RayMaterial material;
        material.albedoCoverage = float4(clamp(albedo, 0.0, 1.0), meanCoverage);
        material.emissionFlags = float4(sharedEmission[0].rgb / 256.0, asfloat(constants.params.z));
        material.samplers = constants.samplers;
        rayMaterials[constants.params.x] = material;
    }
}
