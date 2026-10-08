// ReSTIR PT's temporal reuse (restir_pt_common.slang): the pixel's initial reservoir resampled with last
// frame's final reservoir where its surface was, found through the motion vectors, or where a
// disocclusion makes that a different surface, along the occluder's own motion (dual motion vectors,
// Zeng et al. 2021; the paper's section 6.4). The history's confidence is capped by how many of its
// neighbours shared its sample (the duplication map, section 5). Both paths are shifted into the other's
// domain for the pairwise MIS weights: the initial one onto last frame's surface as last frame's camera
// saw it. In place in the working reservoirs.

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

// The history pixel whose surface matches s, -1 for none.
int FindTemporalNeighbour(int2 pixel, PtSurface s, float2 velocity, out PtSurface previous)
{
    previous = PtInvalidSurface();
    float2 uv = (float2(pixel) + 0.5) / float2(pt.extent);
    int2 q = int2(floor((uv - velocity) * float2(pt.extent)));
    // Permutation sampling (RTXDI): this frame's history pixel is a neighbour in the 2 x 2 quad, the
    // same swap for every pixel (a permutation: each history reservoir is still reused once). A pixel
    // then no longer resamples its own history frame after frame, so a sample does not sit still on
    // screen for its whole life, which DLSS ray reconstruction would keep as texture.
    if (PtFlag(PT_FLAG_PERMUTATION))
    {
        uint permutation = PtHash(pt.frameIndex * 0x9E3779B1u + 0x2545f491u);
        q ^= int2(int(permutation & 1u), int((permutation >> 1) & 1u));
    }
    if (!PtInside(q))
    {
        return -1;
    }
    float2 occluderVelocity;
    previous = PtLoadSurface(PtPixelIndex(q), true, occluderVelocity);
    if (PtSimilarSurfaces(s, previous, pt.previousCamera.xyz, 0.9, 0.05))
    {
        return int(PtPixelIndex(q));
    }
    if (!PtFlag(PT_FLAG_DUAL_MOTION) || !previous.valid)
    {
        return -1;
    }
    // A disocclusion: what was in front last frame moved by its own motion; the surface uncovered now
    // lay beside it then, as far behind as it moved.
    int2 alternative = int2(floor((uv - occluderVelocity) * float2(pt.extent)));
    if (!PtInside(alternative) || all(alternative == q))
    {
        return -1;
    }
    float2 unused;
    previous = PtLoadSurface(PtPixelIndex(alternative), true, unused);
    return PtSimilarSurfaces(s, previous, pt.previousCamera.xyz, 0.8, 0.1) ? int(PtPixelIndex(alternative)) : -1;
}

float MisWeight(float a, float b)
{
    return a + b > 0.0 ? a / (a + b) : 0.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 pixel = int2(dispatchThreadId.xy);
    if (!PtInside(pixel) || !PtFlag(PT_FLAG_HISTORY_VALID))
    {
        return;
    }
    uint index = PtPixelIndex(pixel);
    float2 velocity;
    PtSurface current = PtLoadSurface(index, false, velocity);
    if (!current.valid)
    {
        return;
    }
    PtSurface previous;
    int historyIndex = FindTemporalNeighbour(pixel, current, velocity, previous);
    if (historyIndex < 0)
    {
        return;
    }
    PtReservoir history = PtLoadReservoir(uint(historyIndex), true);
    if (PtReservoirEmpty(history) || history.M <= 0.0)
    {
        return;
    }
    PtReservoir canonical = PtLoadReservoir(index, false);
    float3 camera = ubo.cameraWorldPosition.xyz;
    float canonicalM = max(canonical.M, 1.0);
    float historyM = min(history.M, PtTemporalCap(uint(historyIndex)));

    // The canonical sample in last frame's domain, and the history's in this pixel's.
    PtShift toPrevious = PtShiftPath(canonical, previous, pt.previousCamera.xyz);
    PtShift toCurrent = PtShiftPath(history, current, camera);

    float canonicalTarget = PtLuminance(canonical.F);
    float canonicalAtPrevious = toPrevious.valid ? PtLuminance(toPrevious.F) * toPrevious.jacobian : 0.0;
    float canonicalWeight = MisWeight(canonicalM * canonicalTarget, historyM * canonicalAtPrevious) * canonicalTarget * canonical.W;

    float historyWeight = 0.0;
    float historyTarget = 0.0;
    if (toCurrent.valid && toCurrent.jacobian > 0.0)
    {
        historyTarget = PtLuminance(toCurrent.F);
        float historyAtPrevious = PtLuminance(history.F) / toCurrent.jacobian;
        historyWeight = MisWeight(historyM * historyAtPrevious, canonicalM * historyTarget) * historyTarget * history.W * toCurrent.jacobian;
    }
    if (!(historyWeight > 0.0) || isinf(historyWeight))
    {
        historyWeight = 0.0;
    }

    float weightSum = canonicalWeight + historyWeight;
    PtReservoir result = canonical;
    float target = canonicalTarget;
    if (historyWeight > 0.0 && PtRandom(PtPathSeed(pixel), 14u, 0u) * weightSum < historyWeight)
    {
        result = history;
        result.F = toCurrent.F;
        result.rcJacobian = toCurrent.rcJacobian;
        target = historyTarget;
    }
    result.W = target > 0.0 ? weightSum / target : 0.0;
    result.M = canonicalM + historyM;
    PtStoreReservoir(index, result, false);
}
