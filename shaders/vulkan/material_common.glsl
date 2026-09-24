#ifndef MATERIAL_COMMON_GLSL
#define MATERIAL_COMMON_GLSL

// One draw's material: GpuMaterialData in engine/renderer/material.h, member for member (std430,
// 6 x vec4). triangle.frag and gbuffer.frag index it with the draw slot triangle.vert forwards.
struct MaterialData
{
    vec4 baseColorFactor;
    vec3 emissiveFactor;
    float alphaCutoff;
    vec4 surfaceFactors;   // x metallic, y roughness, z normal scale, w occlusion strength
    vec4 nodeGraphFactors; // x blend graph enabled, y blend factor
    uvec4 shadingModel;    // x = SHADING_MODEL_* (gbuffer_common.glsl); yzw reserved
    vec4 clearcoatFactors; // x clearcoat factor, y clearcoat roughness; read for SHADING_MODEL_CLEARCOAT
};

layout(set = 0, binding = 12, std430) readonly buffer MaterialBuffer
{
    MaterialData materials[];
}
materialData;

#endif
