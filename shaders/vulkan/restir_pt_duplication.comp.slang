// ReSTIR PT's duplication map (the paper's section 5): for each pixel's final reservoir, the share of the
// 288 others in its 17 x 17 neighbourhood that hold a copy of the same initial sample, told by the path
// seed every shift carries along. Next frame's temporal reuse lowers the history's confidence cap where
// it is high. The seeds of a 32 x 32 tile (the group's 16 x 16 pixels and an 8-pixel apron) are read
// into shared memory once.

#include "scene_common.slang"
#include "gbuffer_common.slang"
#include "pbr_common.slang"
#include "pre_exposure.slang"
#include "ray_tracing_common.slang"
#include "material_common.slang"
#include "material_uv.slang"
#include "ray_hit_common.slang"
#include "restir_pt_common.slang"

static const int RADIUS = 8;
static const int TILE = 32;
groupshared uint seeds[TILE * TILE];

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID, uint groupIndex : SV_GroupIndex, uint3 groupId : SV_GroupID)
{
    int2 groupOrigin = int2(groupId.xy) * 16 - RADIUS;
    for (uint i = groupIndex; i < uint(TILE * TILE); i += 256u)
    {
        int2 p = groupOrigin + int2(int(i) % TILE, int(i) / TILE);
        seeds[i] = PtInside(p) ? historyReservoirs[PtPixelIndex(p) * 4u + 1u].x : 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    int2 pixel = int2(dispatchThreadId.xy);
    if (!PtInside(pixel))
    {
        return;
    }
    int2 local = int2(groupThreadId.xy) + RADIUS;
    uint own = seeds[local.y * TILE + local.x];
    uint copies = 0u;
    if (own != 0u)
    {
        for (int dy = -RADIUS; dy <= RADIUS; ++dy)
        {
            for (int dx = -RADIUS; dx <= RADIUS; ++dx)
            {
                copies += seeds[(local.y + dy) * TILE + local.x + dx] == own ? 1u : 0u;
            }
        }
        copies -= 1u;
    }
    duplication[PtPixelIndex(pixel)] = float(copies) / 288.0;
}
