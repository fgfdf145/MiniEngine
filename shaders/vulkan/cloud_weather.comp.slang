#include "scene_common.slang"

// The volumetric clouds' plume map, rebuilt whenever the plumes' lives move on
// (VulkanAtmosphere::Record; docs/design/2026-10-06-cumulus-generation-design.md,
// 2026-10-07-dynamic-clouds-design.md). One tile of fair-weather cumulus: per
// texel, the top of the tallest plume above the layer's base as a share of the thickness (r,
// stored as (top - WEATHER_FLOOR) / (1 - WEATHER_FLOOR)) and how steeply it falls, |d top / d uv|
// over WEATHER_SLOPE_SCALE (g). The plumes are domes t (1 - (d / r)^2) on four jittered grids, joined by a smooth maximum,
// four times as many per halving of size; the two larger scales are clusters of turrets. Outside
// every plume the top runs on below the base, down to the floor, so the surface distance keeps
// falling away from a cloud's side. Each plume lives on its own phase of its scale's clock
// (ubo.cloudLife): it rises out of the base, stands and sinks back (PlumeLife), and a cluster's
// turrets bob within it. Line for line CloudWeatherTexel in
// engine/renderer/volumetric_clouds.cpp, which the tests measure.
// Set 1 binding 10 of the atmosphere's compute set: the map; binding 14 the plume table, every
// cell of every scale drawn once on the CPU (BuildCloudPlumeTable), so a rebuild only shapes the
// domes for the plumes' current lives.
[[vk::binding(10, 1)]] [[vk::image_format("rg16")]] RWTexture2D<float2> weatherOut;

// Must match CloudPlumeCell in engine/renderer/volumetric_clouds.h.
struct PlumeCell
{
    float4 core;       // xy centre from the cell's corner, z radius (cells), w height; w < 0 no plume
    float4 life;       // x the life's start phase, y turret count
    float4 turrets[5]; // xy centre from the plume's, z radius, w height
    float4 bobs[2];    // each turret's bob start: 0-3 in bobs[0], 4 in bobs[1].x
};
[[vk::binding(14, 1)]] StructuredBuffer<PlumeCell> plumeCells;

// Must match kCloudWeatherSize, kCloudWeatherFloor, kCloudWeatherSlopeScale, kCloudDomeExponent,
// kCloudDomeBlend, kCloudPlumeLevels' cells and kCloudPlumeTableOffsets in
// engine/renderer/volumetric_clouds.h.
static const int WEATHER_SIZE = 1024;
static const float WEATHER_FLOOR = -0.25;
static const float WEATHER_SLOPE_SCALE = 256.0;
static const float DOME_EXPONENT = 2.0;
static const float DOME_BLEND = 0.06;
static const int PLUME_LEVELS = 4;
static const float LEVEL_CELLS[PLUME_LEVELS] = {16.0, 32.0, 64.0, 128.0};
static const int LEVEL_OFFSET[PLUME_LEVELS] = {0, 256, 1280, 5376};
// Must match kCloudLifeGrowth, kCloudLifeDecayStart and kCloudTurretLowest.
static const float LIFE_GROWTH = 0.3;
static const float LIFE_DECAY_START = 0.55;
static const float TURRET_LOWEST = 0.6;
static const float PI = 3.14159265358979323846;

int2 Wrap(int2 cell, int period)
{
    return (cell % period + period) % period;
}

// How much of its full height a plume has at a phase of its life: rising, standing, sinking away.
float PlumeLife(float phase)
{
    float p = frac(phase);
    return smoothstep(0.0, LIFE_GROWTH, p) * (1.0 - smoothstep(LIFE_DECAY_START, 1.0, p));
}

// One dome of radius and height (grid units, share of the thickness) at centre, sunk by the share
// 1 - life of its height, joined to the top so far by the smooth maximum; its slope per uv follows.
void WeatherDome(float2 grid, float2 centre, float radius, float height, float life, float cells, inout float top, inout float slope)
{
    float d = length(grid - centre) / radius;
    float dome = height * (life - pow(d, DOME_EXPONENT));
    float domeSlope = height * DOME_EXPONENT * pow(d, DOME_EXPONENT - 1.0) / radius * cells;
    // Polynomial smooth maximum; the slope follows the blend.
    float blend = clamp(0.5 + 0.5 * (dome - top) / DOME_BLEND, 0.0, 1.0);
    top = lerp(top, dome, blend) + DOME_BLEND * blend * (1.0 - blend);
    slope = lerp(slope, domeSlope, blend);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 texel = int2(dispatchThreadId.xy);
    if (any(texel >= int2(WEATHER_SIZE)))
    {
        return;
    }
    float2 uv = (float2(texel) + 0.5) / float(WEATHER_SIZE);
    float top = WEATHER_FLOOR;
    float slope = 0.0;
    for (int level = 0; level < PLUME_LEVELS; ++level)
    {
        float cellsF = LEVEL_CELLS[level];
        int cells = int(cellsF);
        float2 grid = uv * cellsF;
        int2 base = int2(floor(grid));
        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                int2 cell = base + int2(dx, dy);
                int2 wrapped = Wrap(cell, cells);
                int index = LEVEL_OFFSET[level] + wrapped.y * cells + wrapped.x;
                float4 core = plumeCells[index].core;
                if (core.w < 0.0)
                {
                    continue;
                }
                float4 lifeInfo = plumeCells[index].life;
                float2 centre = float2(cell) + core.xy;
                float phase = frac(ubo.cloudLife[level] + lifeInfo.x);
                float life = PlumeLife(phase);
                WeatherDome(grid, centre, core.z, core.w, life, cellsF, top, slope);
                int turrets = int(lifeInfo.y);
                for (int turret = 0; turret < turrets; ++turret)
                {
                    float4 shape = plumeCells[index].turrets[turret];
                    float bob = 0.5 + 0.5 * cos(2.0 * PI * frac(2.0 * phase + plumeCells[index].bobs[turret / 4][turret % 4]));
                    WeatherDome(grid, centre + shape.xy, shape.z, shape.w, life * lerp(TURRET_LOWEST, 1.0, bob), cellsF, top, slope);
                }
            }
        }
    }
    top = max(top, WEATHER_FLOOR);
    weatherOut[texel] = float2((top - WEATHER_FLOOR) / (1.0 - WEATHER_FLOOR), min(slope / WEATHER_SLOPE_SCALE, 1.0));
}
