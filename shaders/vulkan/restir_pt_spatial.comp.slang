// The second half of ReSTIR PT's paired spatial reuse, and the frame's last ReSTIR step: the pixel's
// reservoir (canonical) resampled with its partners' in the three pairing textures, through the shifts
// restir_pt_spatial_shift.comp left (each partner's sample onto this surface, this sample onto each
// partner's), with the generalised pairwise MIS weights and confidences:
//   m_i(y) = s_i M_i p_i(y) / (M_i p_i(y) + (M_c / k) p_c(y)),  s_i = (M_i + M_c / k) / M_total,
//   m_c(x) = sum_i s_i (M_c / k) p_c(x) / (M_i p_i(x) + (M_c / k) p_c(x)),
// where p_i(y) is the partner's target carried into this domain. The pixel is shaded with the
// vector-valued weights sum m F W |J| (the paper's section 6.3) and the chosen sample becomes next
// frame's history. With spatial reuse off it only passes the working reservoir on.

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
    if (!PtSurfaceValid(index))
    {
        PtReservoir empty = PtEmptyReservoir();
        PtStoreReservoir(index, empty, true);
        PtWriteOutput(pixel, float3(0.0), empty);
        return;
    }
    if (!PtFlag(PT_FLAG_SPATIAL))
    {
        PtReservoir canonical = PtLoadReservoir(index, false);
        PtStoreReservoir(index, canonical, true);
        PtWriteOutput(pixel, canonical.F * canonical.W, canonical);
        return;
    }

    // The partners the shift pass accepted (both ends of a pair agree), and the two shifts of each pair.
    int2 partners[PT_SPATIAL_NEIGHBOURS];
    bool included[PT_SPATIAL_NEIGHBOURS];
    uint4 mineShifts[PT_SPATIAL_NEIGHBOURS];
    uint count = 0u;
    for (uint t = 0u; t < PT_SPATIAL_NEIGHBOURS; ++t)
    {
        partners[t] = PtPartner(pixel, t);
        mineShifts[t] = spatialShifts[index * PT_SPATIAL_NEIGHBOURS + t];
        included[t] = PtShiftAccepted(mineShifts[t]);
        count += included[t] ? 1u : 0u;
    }

    // The reservoirs' heads only: the reconnection vertex is read for the chosen sample alone.
    PtReservoir canonical = PtLoadWorkReservoirHead(index);
    PtReservoir neighbours[PT_SPATIAL_NEIGHBOURS];
    float totalM = canonical.M;
    for (uint t = 0u; t < PT_SPATIAL_NEIGHBOURS; ++t)
    {
        if (included[t])
        {
            neighbours[t] = PtLoadWorkReservoirHead(PtPixelIndex(partners[t]));
            totalM += neighbours[t].M;
        }
    }

    uint seed = PtPathSeed(pixel);
    float canonicalShare = count > 0u ? canonical.M / float(count) : canonical.M;
    float canonicalTarget = PtLuminance(canonical.F);
    float canonicalMis = count == 0u ? 1.0 : 0.0;
    float weightSum = 0.0;
    float3 color = float3(0.0);
    PtReservoir result = canonical;
    float resultTarget = canonicalTarget;
    // The partner whose sample is chosen, and its shift; -1 for the canonical sample.
    int chosen = -1;
    PtShift chosenShift;
    chosenShift.valid = false;

    for (uint t = 0u; t < PT_SPATIAL_NEIGHBOURS; ++t)
    {
        if (!included[t])
        {
            continue;
        }
        uint partnerIndex = PtPixelIndex(partners[t]);
        PtReservoir neighbour = neighbours[t];
        float share = totalM > 0.0 ? (neighbour.M + canonicalShare) / totalM : 0.0;

        // This sample in the partner's domain: the canonical's weight for this pair.
        PtShift mine = PtUnpackShift(mineShifts[t], canonical.rcJacobian);
        float partnerAtMine = mine.valid ? PtLuminance(mine.F) * mine.jacobian : 0.0;
        float denominator = canonicalShare * canonicalTarget + neighbour.M * partnerAtMine;
        canonicalMis += denominator > 0.0 ? share * canonicalShare * canonicalTarget / denominator : share;

        // The partner's sample in this domain.
        PtShift theirs = PtUnpackShift(spatialShifts[partnerIndex * PT_SPATIAL_NEIGHBOURS + t], neighbour.rcJacobian);
        if (!theirs.valid || theirs.jacobian <= 0.0 || PtReservoirEmpty(neighbour) || neighbour.W <= 0.0)
        {
            continue;
        }
        float target = PtLuminance(theirs.F);
        float partnerTarget = PtLuminance(neighbour.F) / theirs.jacobian;
        float pairDenominator = neighbour.M * partnerTarget + canonicalShare * target;
        float mis = pairDenominator > 0.0 ? share * neighbour.M * partnerTarget / pairDenominator : 0.0;
        float ucw = neighbour.W * theirs.jacobian;
        float weight = mis * target * ucw;
        if (!(weight > 0.0) || isinf(weight))
        {
            continue;
        }
        color += mis * theirs.F * ucw;
        weightSum += weight;
        if (PtRandom(seed, 13u, t) * weightSum < weight)
        {
            chosen = int(t);
            chosenShift = theirs;
            resultTarget = target;
        }
    }

    float canonicalWeight = canonicalMis * canonicalTarget * canonical.W;
    if (canonicalWeight > 0.0 && !isinf(canonicalWeight))
    {
        color += canonicalMis * canonical.F * canonical.W;
        weightSum += canonicalWeight;
        if (PtRandom(seed, 13u, 7u) * weightSum < canonicalWeight)
        {
            chosen = -1;
            resultTarget = canonicalTarget;
        }
    }

    if (chosen >= 0)
    {
        result = PtLoadReservoir(PtPixelIndex(partners[chosen]), false);
        result.F = chosenShift.F;
        result.rcJacobian = chosenShift.rcJacobian;
    }
    else
    {
        result = PtLoadReservoir(index, false);
    }
    result.W = resultTarget > 0.0 ? weightSum / resultTarget : 0.0;
    result.M = totalM;
    PtStoreReservoir(index, result, true);
    PtWriteOutput(pixel, PtFlag(PT_FLAG_COLOR_NOISE) ? color : result.F * result.W, result);
}
