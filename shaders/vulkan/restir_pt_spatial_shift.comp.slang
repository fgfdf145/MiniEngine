// The first half of ReSTIR PT's paired spatial reuse (the paper's section 3): each pixel's sample shifted
// onto the surface of its partner in each pairing texture. The partner reuses this pixel's sample with
// it, and this pixel's canonical MIS weight needs the same shift, so one shift per pair serves both
// directions' halves. A partner off screen or unlike this pixel's surface (a symmetric test) gets no
// shift, and the pair is not reused either way.

#define RAY_TEXTURED_ALPHA
#include "scene_common.slang"
#include "gbuffer_common.slang"
#include "pbr_common.slang"
#include "pre_exposure.slang"
#include "ray_tracing_common.slang"
#include "material_common.slang"
#include "material_uv.slang"
#include "ray_hit_common.slang"
#include "restir_pt_common.slang"

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 pixel = int2(dispatchThreadId.xy);
    if (!PtInside(pixel))
    {
        return;
    }
    uint index = PtPixelIndex(pixel);
    float2 velocity;
    PtSurface surface = PtLoadSurface(index, false, velocity);
    PtReservoir reservoir = PtLoadReservoir(index, false);
    float3 camera = ubo.cameraWorldPosition.xyz;
    for (uint t = 0u; t < PT_SPATIAL_NEIGHBOURS; ++t)
    {
        PtShift shift;
        shift.valid = false;
        bool accepted = false;
        int2 partner = PtPartner(pixel, t);
        if (surface.valid && PtInside(partner))
        {
            PtSurface partnerSurface = PtLoadSurface(PtPixelIndex(partner), false, velocity);
            accepted = PtSimilarSurfaces(surface, partnerSurface, camera, 0.9, 0.05);
            if (accepted)
            {
                shift = PtShiftPath(reservoir, partnerSurface, camera);
            }
        }
        spatialShifts[index * PT_SPATIAL_NEIGHBOURS + t] = PtPackShift(shift, accepted);
    }
}
