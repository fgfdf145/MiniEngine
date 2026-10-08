// The local lights' world grid (VulkanPathTraceLights, docs/design/
// 2026-10-08-path-tracing-remaining-work-design.md): cells LIGHT_GRID_CELL metres on a side in a block
// around the camera, each listing up to LIGHT_GRID_SLOTS of the local lights whose range reaches it
// (a uniform random choice of them when more do, made again every frame) and how many do. The path
// tracer's next event estimation draws its local light candidates from the cell of each vertex, where
// the view frustum's cluster grid does not reach or is coarse, in place of the whole scene's lights
// (RTXDI's light tiles and ReGIR in their simplest, unweighted form). Rebuilt every frame by
// light_grid.comp. In the set EMISSIVE_LIGHT_SET, binding 5 (emissive_lights_common.glsl's set);
// the building shader defines EMISSIVE_LIGHTS_WRITE.
//
// Include after scene_common.glsl and emissive_lights_common.glsl.

#ifndef LIGHT_GRID_COMMON_GLSL
#define LIGHT_GRID_COMMON_GLSL

// Must match kLightGrid* in engine/renderer/vulkan/path_trace_lights.cpp.
const uvec3 LIGHT_GRID_DIMS = uvec3(64u, 16u, 64u);
const float LIGHT_GRID_CELL = 8.0;
const uint LIGHT_GRID_SLOTS = 32u;
// Per cell: the count of lights reaching it, then the slots, two 16-bit light indices a word.
const uint LIGHT_GRID_STRIDE = 1u + LIGHT_GRID_SLOTS / 2u;

layout(std430, set = EMISSIVE_LIGHT_SET, binding = 5) EMISSIVE_ACCESS buffer LightGrid
{
    uint lightGrid[];
};

// The grid's lowest corner: centred on the camera, in whole cells so a cell keeps its place as the
// camera moves.
vec3 LightGridOrigin()
{
    return (floor(ubo.cameraWorldPosition.xyz / LIGHT_GRID_CELL) - vec3(LIGHT_GRID_DIMS / 2u)) * LIGHT_GRID_CELL;
}

// The cell a point falls in, false outside the grid.
bool LightGridCellOf(vec3 position, out uint cell)
{
    vec3 local = (position - LightGridOrigin()) / LIGHT_GRID_CELL;
    cell = 0u;
    if (any(lessThan(local, vec3(0.0))) || any(greaterThanEqual(local, vec3(LIGHT_GRID_DIMS))))
    {
        return false;
    }
    uvec3 coordinate = uvec3(local);
    cell = (coordinate.y * LIGHT_GRID_DIMS.z + coordinate.z) * LIGHT_GRID_DIMS.x + coordinate.x;
    return true;
}

#ifndef EMISSIVE_LIGHTS_WRITE
// How many local lights reach the cell (its slots hold min(count, LIGHT_GRID_SLOTS) of them).
uint LightGridCount(uint cell)
{
    return lightGrid[cell * LIGHT_GRID_STRIDE];
}

// The scene light index (into sceneLights.lights) in one of the cell's slots.
uint LightGridLight(uint cell, uint slot)
{
    uint word = lightGrid[cell * LIGHT_GRID_STRIDE + 1u + (slot >> 1u)];
    return (slot & 1u) != 0u ? word >> 16u : word & 0xffffu;
}
#endif

#endif
