// The local lights' world grid (light_grid_common.slang), one thread a cell: every local light whose
// range reaches the cell's box, kept as a uniform random subset of LIGHT_GRID_SLOTS (reservoir
// sampling, so each light is in it with the same probability) and counted. The lights come through
// shared memory a workgroup's worth at a time.

#define EMISSIVE_LIGHT_SET 2
#define EMISSIVE_LIGHTS_WRITE
#include "scene_common.slang"
#include "light_grid_common.slang"

// The scene's lights, as pbr_common.slang declares them (set 0 binding 10).
[[vk::binding(10, 0)]] StructuredBuffer<SceneLightData> sceneLights;

// EmissiveBuildConstants in engine/renderer/vulkan/path_trace_lights.cpp; frame carries the frame
// index here, which reseeds the subsets.
struct LightGridConstants
{
    uint mode;
    uint count;
    uint frame;
    uint depth;
};
[[vk::push_constant]] ConstantBuffer<LightGridConstants> build;

groupshared float4 sharedSpheres[64];

uint Hash(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID)
{
    uint cell = dispatchThreadId.x;
    uint cellCount = LIGHT_GRID_DIMS.x * LIGHT_GRID_DIMS.y * LIGHT_GRID_DIMS.z;
    bool inGrid = cell < cellCount;
    uint3 coordinate = uint3(cell % LIGHT_GRID_DIMS.x, cell / (LIGHT_GRID_DIMS.x * LIGHT_GRID_DIMS.z), (cell / LIGHT_GRID_DIMS.x) % LIGHT_GRID_DIMS.z);
    float3 boxMin = LightGridOrigin() + float3(coordinate) * LIGHT_GRID_CELL;
    float3 boxMax = boxMin + float3(LIGHT_GRID_CELL);

    uint slots[LIGHT_GRID_SLOTS];
    for (uint slot = 0u; slot < LIGHT_GRID_SLOTS; ++slot)
    {
        slots[slot] = 0u;
    }
    uint reaching = 0u;
    uint first = ubo.lightCounts.x;
    uint total = ubo.lightCounts.y;
    for (uint base = first; base < total; base += 64u)
    {
        uint index = base + groupThreadId.x;
        sharedSpheres[groupThreadId.x] = index < total ? sceneLights[index].positionAndRange : float4(0.0, 0.0, 0.0, -1.0);
        GroupMemoryBarrierWithGroupSync();
        uint chunk = min(64u, total - base);
        for (uint k = 0u; inGrid && k < chunk; ++k)
        {
            float4 sphere = sharedSpheres[k];
            float3 away = sphere.xyz - clamp(sphere.xyz, boxMin, boxMax);
            if (sphere.w <= 0.0 || dot(away, away) > sphere.w * sphere.w)
            {
                continue;
            }
            // Algorithm R: the first ones fill the slots, the n-th replaces a random one with
            // probability slots / n.
            if (reaching < LIGHT_GRID_SLOTS)
            {
                slots[reaching] = base + k;
            }
            else
            {
                uint pick = Hash(cell * 0x9e3779b9u ^ Hash(reaching + build.frame * 0x632be5abu)) % (reaching + 1u);
                if (pick < LIGHT_GRID_SLOTS)
                {
                    slots[pick] = base + k;
                }
            }
            ++reaching;
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (!inGrid)
    {
        return;
    }
    uint offset = cell * LIGHT_GRID_STRIDE;
    lightGrid[offset] = reaching;
    for (uint pair = 0u; pair < LIGHT_GRID_SLOTS / 2u; ++pair)
    {
        lightGrid[offset + 1u + pair] = (slots[2u * pair] & 0xffffu) | (slots[2u * pair + 1u] << 16u);
    }
}
