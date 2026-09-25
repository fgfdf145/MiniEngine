#ifndef MATERIAL_UV_GLSL
#define MATERIAL_UV_GLSL

// Where each material texture is sampled: KHR_texture_transform and the textureInfo's UV set, per
// draw slot and texture slot (GpuTextureTransforms in engine/renderer/material.h, uploaded to set 0
// binding 17). A material with no transform (shadingModel.y == 0) samples the first UV set as it is
// and never reads the buffer. Needs material_common.glsl.

#define MATERIAL_TEXTURE_SLOT_COUNT 23u

layout(set = 0, binding = 17, std430) readonly buffer MaterialTextureTransformBuffer
{
    // Two rows per slot: (a, b, tx, UV set) and (c, d, ty, 0).
    vec4 rows[];
}
materialTextureTransforms;

vec2 MaterialSlotUv(MaterialData material, uint drawSlot, uint slot, vec2 uv0, vec2 uv1)
{
    if (material.shadingModel.y == 0u)
        return uv0;
    uint base = (drawSlot * MATERIAL_TEXTURE_SLOT_COUNT + slot) * 2u;
    vec4 row0 = materialTextureTransforms.rows[base];
    vec4 row1 = materialTextureTransforms.rows[base + 1u];
    vec3 uv = vec3(row0.w > 0.5 ? uv1 : uv0, 1.0);
    return vec2(dot(row0.xyz, uv), dot(row1.xyz, uv));
}

// A tangent-space vector (a normal map's xy, an anisotropy direction) read through a rotated
// texture points along the texture's rotated axes; this turns it back into the mesh's tangent
// frame. The rotation comes out of the first column, (cos, -sin) * scale.x, so a negative u scale
// also flips it, as it flips the texture.
vec2 RotateMaterialTangentXy(MaterialData material, uint drawSlot, uint slot, vec2 xy)
{
    if (material.shadingModel.y == 0u)
        return xy;
    uint base = (drawSlot * MATERIAL_TEXTURE_SLOT_COUNT + slot) * 2u;
    vec2 cs = vec2(materialTextureTransforms.rows[base].x, -materialTextureTransforms.rows[base + 1u].x);
    float scale = length(cs);
    if (scale < 1e-8)
        return xy;
    cs /= scale;
    return vec2(cs.x * xy.x - cs.y * xy.y, cs.y * xy.x + cs.x * xy.y);
}

#endif
