#ifndef MATERIAL_COMMON_GLSL
#define MATERIAL_COMMON_GLSL

// One draw's material: GpuMaterialData in engine/renderer/material.h, member for member (std430,
// 9 x vec4). triangle.frag and gbuffer.frag index it with the draw slot triangle.vert forwards.
struct MaterialData
{
    vec4 baseColorFactor;
    vec3 emissiveFactor;
    float alphaCutoff;
    vec4 surfaceFactors;   // x metallic, y roughness, z normal scale, w occlusion strength
    vec4 nodeGraphFactors; // x blend graph enabled, y blend factor
    uvec4 shadingModel;      // x = SHADING_FLAG_* bits (gbuffer_common.glsl); yzw reserved
    vec4 clearcoatFactors;   // x factor, y roughness, z coat normal scale; read with SHADING_FLAG_CLEARCOAT
    vec4 sheenFactors;       // rgb sheen colour, a sheen roughness; read with SHADING_FLAG_SHEEN
    vec4 anisotropyFactors;  // x strength, y cos rotation, z sin rotation; read with SHADING_FLAG_ANISOTROPY
    vec4 specularFactors;    // rgb IOR F0 times the colour factor, a specular factor; read with SHADING_FLAG_SPECULAR
};

layout(set = 0, binding = 12, std430) readonly buffer MaterialBuffer
{
    MaterialData materials[];
}
materialData;

#endif
