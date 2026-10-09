#ifndef MATERIAL_UV_SLANG
#define MATERIAL_UV_SLANG

// Where each material texture is sampled: KHR_texture_transform and the textureInfo's UV set, per
// draw slot and texture slot (GpuTextureTransforms in engine/renderer/material.h, uploaded to set 0
// binding 17). A material with no transform (shadingModel.y == 0) samples the first UV set as it is
// and never reads the buffer. Needs material_common.slang.

#define MATERIAL_TEXTURE_SLOT_COUNT 27u

// Two rows per slot: (a, b, tx, UV set) and (c, d, ty, 0).
[[vk::binding(17, 0)]] StructuredBuffer<float4> materialTextureTransforms : register(t17, space0);

float2 MaterialSlotUv(MaterialData material, uint drawSlot, uint slot, float2 uv0, float2 uv1)
{
    if (material.shadingModel.y == 0u)
        return uv0;
    uint base = (drawSlot * MATERIAL_TEXTURE_SLOT_COUNT + slot) * 2u;
    float4 row0 = materialTextureTransforms[base];
    float4 row1 = materialTextureTransforms[base + 1u];
    float3 uv = float3(row0.w > 0.5 ? uv1 : uv0, 1.0);
    return float2(dot(row0.xyz, uv), dot(row1.xyz, uv));
}

// A tangent-space vector (a normal map's xy, an anisotropy direction) read through a rotated
// texture points along the texture's rotated axes; this turns it back into the mesh's tangent
// frame. The rotation comes out of the first column, (cos, -sin) * scale.x, so a negative u scale
// also flips it, as it flips the texture.
float2 RotateMaterialTangentXy(MaterialData material, uint drawSlot, uint slot, float2 xy)
{
    if (material.shadingModel.y == 0u)
        return xy;
    uint base = (drawSlot * MATERIAL_TEXTURE_SLOT_COUNT + slot) * 2u;
    float2 cs = float2(materialTextureTransforms[base].x, -materialTextureTransforms[base + 1u].x);
    float scale = length(cs);
    if (scale < 1e-8)
        return xy;
    cs /= scale;
    return float2(cs.x * xy.x - cs.y * xy.y, cs.y * xy.x + cs.x * xy.y);
}

#endif
