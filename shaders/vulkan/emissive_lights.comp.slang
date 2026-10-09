// The emissive light list's frame (VulkanPathTraceLights, emissive_lights_common.slang), in three kinds
// of dispatch:
//   0, per instance: which instance draws each emissive slot this frame (instances move in the top
//      level's order, and a moved instance's old leaf stays behind, skipped);
//   1, per leaf of the power tree: each light's triangle in world space, from its instance's transform,
//      and its power, the emission at the triangle's centre at the mip that averages the triangle,
//      times its area (twice for a double-sided material); 0 for the padding and for lights no instance
//      draws;
//   2, per node of one level: the sum of its four children, from the level below up to the root.

#define RAY_SCENE_SET 1
#define EMISSIVE_LIGHT_SET 2
#define RAY_TEXTURE_SET 3
#define EMISSIVE_LIGHTS_WRITE
#include "shader_helpers.slang"
#include "scene_common.slang"
#include "ray_tracing_common.slang"
#include "material_common.slang"
#include "material_uv.slang"
#include "ray_hit_common.slang"
#include "emissive_lights_common.slang"

// EmissiveBuildConstants in engine/renderer/vulkan/path_trace_lights.cpp.
struct EmissiveBuildConstants
{
    uint mode;
    // Mode 0: the instances; 1: the lights; 2: the level being summed.
    uint count;
    uint level;
    uint depth;
};
[[vk::push_constant]] ConstantBuffer<EmissiveBuildConstants> build;

void SetTreeValue(uint index, float value)
{
    emissiveTree[index >> 2u][index & 3u] = value;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (build.mode == 0u)
    {
        if (index >= build.count)
        {
            return;
        }
        RayInstance instance = rayInstances[index];
        uint slot = instance.data.z;
        if ((instance.data.w & RAY_INSTANCE_SKIP) == 0u && slot < BufferLength(emissiveSlotBase) && emissiveSlotBase[slot] != ~0u)
        {
            emissiveSlotInstance[slot] = index;
        }
        return;
    }

    if (build.mode == 2u)
    {
        if (index >= (1u << (2u * build.level)))
        {
            return;
        }
        float4 children = emissiveTree[(EmissiveTreeLevelStart(build.level + 1u) >> 2u) + index];
        float sum = children.x + children.y + children.z + children.w;
        if (build.level == 0u)
        {
            emissiveTree[0].x = sum;
        }
        else
        {
            SetTreeValue(EmissiveTreeLevelStart(build.level) + index, sum);
        }
        return;
    }

    uint leafCount = 1u << (2u * build.depth);
    if (index >= leafCount)
    {
        return;
    }
    if (index == 0u)
    {
        emissiveTree[0].y = asfloat(build.depth);
        emissiveTree[0].z = asfloat(build.count);
    }
    float power = 0.0;
    if (index < build.count)
    {
        uint2 entry = emissiveEntries[index];
        uint instanceIndex = emissiveSlotInstance[entry.x];
        EmissiveTriangle light;
        light.v0Lod = float4(0.0);
        light.e1Slot = float4(0.0, 0.0, 0.0, asfloat(entry.x));
        light.e2Front = float4(0.0);
        light.uv01 = float4(0.0);
        light.uv2 = float4(0.0);
        light.emission = float4(0.0);
        if (instanceIndex != ~0u)
        {
            RayInstance instance = rayInstances[instanceIndex];
            uint leaf = instance.data.y + entry.y;
            BvhTriangle triangle = meshTriangles[leaf];
            // worldToObject's rows: object = M world + t, so world = M^-1 (object - t); the front side's
            // normal is transpose(M) times the object one (RayHitNormal).
            float3x3 M = MatrixFromRows(instance.worldToObject[0].xyz, instance.worldToObject[1].xyz, instance.worldToObject[2].xyz);
            float3 t = float3(instance.worldToObject[0].w, instance.worldToObject[1].w, instance.worldToObject[2].w);
            float3x3 inverseM = Inverse(M);
            float3 v0 = mul(inverseM, triangle.v0.xyz - t);
            float3 e1 = mul(inverseM, triangle.e1.xyz);
            float3 e2 = mul(inverseM, triangle.e2.xyz);
            float3 front = mul(transpose(M), cross(triangle.e1.xyz, triangle.e2.xyz));
            float3 crossed = cross(e1, e2);
            uint materialFlags = asuint(rayMaterials[entry.x].emissionFlags.w);
            bool doubleSided = (materialFlags & RAY_MATERIAL_DOUBLE_SIDED) != 0u;
            float side = doubleSided ? 0.0 : (dot(crossed, front) >= 0.0 ? 1.0 : -1.0);
            // The emission map's coordinates at the corners, through the material's transform.
            MaterialData material = materials[entry.x];
            RayHitCorners corners = RayHitCornersOf(instanceIndex, leaf);
            float2 uvA = MaterialSlotUv(material, entry.x, 5u, RayCornerVec2(corners, 0u, RAY_VERTEX_UV0), RayCornerVec2(corners, 0u, RAY_VERTEX_UV1));
            float2 uvB = MaterialSlotUv(material, entry.x, 5u, RayCornerVec2(corners, 1u, RAY_VERTEX_UV0), RayCornerVec2(corners, 1u, RAY_VERTEX_UV1));
            float2 uvC = MaterialSlotUv(material, entry.x, 5u, RayCornerVec2(corners, 2u, RAY_VERTEX_UV0), RayCornerVec2(corners, 2u, RAY_VERTEX_UV1));
            uint textureIndex = entry.x * RAY_TEXTURES_PER_SLOT + RAY_TEXTURE_EMISSIVE;
            float2 size = float2(TextureSize(rayTextures[NonUniformResourceIndex(textureIndex)], 0));
            float2 edgeB = uvB - uvA;
            float2 edgeC = uvC - uvA;
            float texels = abs(edgeB.x * edgeC.y - edgeB.y * edgeC.x) * size.x * size.y;
            float area = 0.5 * length(crossed);
            light.v0Lod = float4(v0, 0.5 * log2(max(texels, 1e-12) / max(area, 1e-12)));
            light.e1Slot.xyz = e1;
            light.e2Front = float4(e2, side);
            light.uv01 = float4(uvA, uvB);
            light.uv2 = float4(uvC, 0.0, 0.0);
            light.emission = float4(material.emissiveFactor, 0.0);

            // The power: the emission at the centre, at the mip where the triangle is about one texel.
            float lod = max(0.5 * log2(max(texels, 1e-12)), 0.0);
            float3 emission = RayTextureSampleLevel(entry.x, RAY_TEXTURE_EMISSIVE, (uvA + uvB + uvC) / 3.0, lod).rgb * material.emissiveFactor;
            power = dot(max(emission, float3(0.0)), float3(0.2126, 0.7152, 0.0722)) * area * (doubleSided ? 2.0 : 1.0);
            if (isnan(power) || isinf(power))
            {
                power = 0.0;
            }
        }
        emissiveTriangles[index] = light;
    }
    SetTreeValue(EmissiveTreeLevelStart(build.depth) + index, power);
}
