// Blends one scheduled probe's rays (ddgi_trace.comp) into its atlas tiles: one workgroup per probe,
// one invocation per visibility texel, the first 64 also one per irradiance texel. Each texel's
// direction takes the rays' average weighted by the cosine (irradiance: radiance, and the share of
// the lobe that escaped to the sky) or a sharp power of it (visibility: the distance and its square,
// clamped to twice the spacing), then keeps a hysteresis share of what the texel held. A probe whose
// record is for another coordinate (a scroll moved its slot) or that was never updated keeps nothing
// of its own; its irradiance starts halfway from the next coarser level's.
// The border texels copy the octahedron's opposite edges, so bilinear lookups wrap correctly.
//
// The same rays relocate and classify the probe (Majercik et al. 2021): a probe with more than a
// quarter of its rays on back faces is inside geometry and moves toward the open space it sees; a probe
// closer to a surface than a fifth of the spacing moves away from it; the offset stays within 0.45
// spacing of the grid point. A probe is inactive where it sees a back face within one spacing: it is
// inside something, or on the wrong side of a single-sided surface (under a floor or a terrain, where
// it would see the sky past the building's edge and light the rooms above). That evidence is smoothed
// over updates (offset.w), so the rotating rays do not flip the probe on and off.
//
// A probe's light that changes (a door opened, a lamp switched on, new geometry streamed in) restarts
// its average instead of creeping toward the new value at the hysteresis' 3% an update: the update
// compares its rays' average luminance with the running mean of the probe's past updates and treats a
// difference beyond four standard deviations of their spread (and a tenth of the mean) as a change.
// So does a lighting epoch other than the one the probe recorded (the sun or the sky changed). Either
// way the update keeps half of what the probe held and the count of averaged updates starts over at 1,
// so the next ones keep 2/3, 3/4 and so on up to the hysteresis; a change by more than
// RESTART_RATIO either way keeps nothing: the n-th update after it still holds 1 / (n + 1) of the old
// light, and from a sunlit afternoon to moonlight (some 10^5 times darker) that was thousands of
// updates of daylight in a night scene. The CPU learns of it through the feedback buffer and updates
// the probe and its neighbours again soon (DdgiProbeScheduler).

#include "shader_helpers.slang"
#include "scene_common.slang"
// The update writes one level of the atlases while it samples the next coarser one, so it reads
// them through its own set, in GENERAL, not set 0's views of every level, which rest in
// SHADER_READ_ONLY_OPTIMAL.
#define DDGI_UPDATE_ATLASES
[[vk::binding(6, 2)]] Texture2DArray ddgiIrradianceAtlas;
[[vk::binding(7, 2)]] Texture2DArray ddgiVisibilityAtlas;
[[vk::binding(8, 2)]] SamplerState ddgiIrradianceAtlasSampler;
[[vk::binding(9, 2)]] SamplerState ddgiVisibilityAtlasSampler;
#include "ddgi_common.slang"

[[vk::binding(0, 2)]] StructuredBuffer<uint> scheduledProbes;
[[vk::binding(1, 2)]] StructuredBuffer<float4> rays;
[[vk::binding(2, 2)]] [[vk::image_format("rgba16f")]] RWTexture2DArray<float4> irradianceImage;
[[vk::binding(3, 2)]] [[vk::image_format("rg16f")]] RWTexture2DArray<float2> visibilityImage;
[[vk::binding(4, 2)]] RWStructuredBuffer<DdgiProbeState> probeStatesOut;
// Per scheduled probe: DDGI_FEEDBACK_* bits and the coordinate mod 256 (this frame slot's buffer).
[[vk::binding(5, 2)]] RWStructuredBuffer<uint> feedback;

// Must match DdgiUpdateConstants in engine/renderer/vulkan/ddgi.cpp.
struct DdgiUpdateConstants
{
    float4 rotation[3];
    uint scheduledCount;
    float hysteresis;
    // The lighting epoch (bits 0 to 7) and the geometry epoch (bits 8 to 11).
    uint epochs;
};
[[vk::push_constant]] ConstantBuffer<DdgiUpdateConstants> constants;

static const float PI = 3.14159265359;
static const float DDGI_MISS_DISTANCE = 1e27;
// The visibility lobe's power: sharp, so a texel's distance is the one straight along it.
static const float VISIBILITY_SHARPNESS = 50.0;
// A change in the probe's light: its updates' average luminance this far from their running mean, in
// standard deviations of their spread, plus this share of the mean; and only once this many updates
// have built the statistics.
static const float CHANGE_SIGMAS = 4.0;
static const float CHANGE_RELATIVE = 0.1;
static const int CHANGE_MIN_UPDATES = 4;
// A change by more than this factor either way starts the probe's average over from the new light.
static const float RESTART_RATIO = 2.0;
// Within this many spacings of a surface a probe lights it: the farthest point of the cells it is a
// corner of is sqrt(3) spacings away, plus its relocation of at most 0.45 spacing along each axis.
static const float EMPTY_DISTANCE = 2.5;

groupshared float4 sharedRays[64];
groupshared float3 sharedDirections[64];
groupshared float4 sharedIrradiance[64];
groupshared float2 sharedVisibility[256];
groupshared float sharedLuminance[64];
groupshared float sharedHysteresis;
groupshared bool sharedChanged;

float3 SphericalFibonacci(float index, float count)
{
    const float goldenRatio = 1.61803398875;
    float phi = 2.0 * PI * frac(index * (goldenRatio - 1.0));
    float cosTheta = 1.0 - (2.0 * index + 1.0) / count;
    float sinTheta = sqrt(clamp(1.0 - cosTheta * cosTheta, 0.0, 1.0));
    return float3(cos(phi) * sinTheta, cosTheta, sin(phi) * sinTheta);
}

float3 TexelDirection(int2 texel, int texels)
{
    return DdgiOctDecode(((float2(texel) + 0.5) / float(texels)) * 2.0 - 1.0);
}

// The interior texel a border texel of a (texels + 2)-wide tile copies: the opposite edge, mirrored.
int2 BorderSource(int2 texel, int texels)
{
    int last = texels + 1;
    bool left = texel.x == 0;
    bool right = texel.x == last;
    bool top = texel.y == 0;
    bool bottom = texel.y == last;
    if ((left || right) && (top || bottom))
    {
        return int2(left ? texels : 1, top ? texels : 1);
    }
    if (top || bottom)
    {
        return int2(last - texel.x, top ? 1 : texels);
    }
    return int2(left ? 1 : texels, last - texel.y);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 groupThreadId : SV_GroupThreadID, uint groupIndex : SV_GroupIndex, uint3 groupId : SV_GroupID)
{
    uint probe = groupId.x;
    if (probe >= constants.scheduledCount)
    {
        return;
    }
    uint thread = groupIndex;
    uint packed = scheduledProbes[probe];
    uint level = packed >> 24;
    int3 slot = DdgiSlotFromIndex(packed & 0xffffffu);
    int3 coord = DdgiSlotCoordinate(slot, DdgiLevelOrigin(level));
    uint stateIndex = level * DDGI_PROBES_PER_LEVEL + DdgiSlotIndex(slot);
    DdgiProbeState state = probeStatesOut[stateIndex];
    bool sameCoordinate = all(state.coordAndFlags.xyz == coord);
    bool fresh = !sameCoordinate || (state.coordAndFlags.w & DDGI_PROBE_UPDATED) == 0 || (state.coordAndFlags.w & DDGI_PROBE_MOVED) != 0;
    int flagsIn = state.coordAndFlags.w;
    uint lightingEpoch = constants.epochs & 0xffu;
    uint geometryEpoch = (constants.epochs >> 8) & 0xfu;
    bool newLighting = (uint(flagsIn >> DDGI_PROBE_LIGHTING_EPOCH_SHIFT) & 0xffu) != lightingEpoch;
    bool newGeometry = (uint(flagsIn >> DDGI_PROBE_GEOMETRY_EPOCH_SHIFT) & 0xfu) != geometryEpoch;
    // A young probe averages its updates (the n-th keeps n / (n + 1) of what it held) until that is
    // more than the hysteresis: what it started from, half the coarser level's light, washes out as
    // 1 / (n + 1) instead of 3% an update. The coarser level leaks light into rooms smaller than its
    // spacing, and at 0.97 alone a probe that scrolled in stayed that bright for hundreds of updates.
    int updateCount = fresh ? 0 : (flagsIn >> DDGI_PROBE_UPDATE_COUNT_SHIFT) & DDGI_PROBE_UPDATE_COUNT_MAX;
    float maxDistance = 2.0 * DdgiLevelSpacing(level);

    if (thread < 64u)
    {
        float3x3 rotation = MatrixFromColumns(constants.rotation[0].xyz, constants.rotation[1].xyz, constants.rotation[2].xyz);
        sharedRays[thread] = rays[probe * 64u + thread];
        sharedDirections[thread] = normalize(mul(rotation, SphericalFibonacci(float(thread), 64.0)));
    }
    GroupMemoryBarrierWithGroupSync();

    int3 layerOrigin = int3(DdgiTile(slot), int(level));
    // Visibility: every invocation, one texel of the 16 x 16 interior.
    int2 visibilityTexel = int2(groupThreadId.xy);
    {
        float3 direction = TexelDirection(visibilityTexel, DDGI_VISIBILITY_TEXELS);
        float2 moments = float2(0.0);
        float total = 0.0;
        for (int ray = 0; ray < 64; ++ray)
        {
            float weight = pow(max(dot(direction, sharedDirections[ray]), 0.0), VISIBILITY_SHARPNESS);
            float distance = min(abs(sharedRays[ray].w), maxDistance);
            moments += weight * float2(distance, distance * distance);
            total += weight;
        }
        moments = total > 1e-6 ? moments / total : float2(maxDistance, maxDistance * maxDistance);
        sharedVisibility[thread] = moments;
    }
    // Irradiance: the first 64 invocations, one texel of the 8 x 8 interior.
    if (thread < 64u)
    {
        int2 irradianceTexel = int2(int(thread) % DDGI_IRRADIANCE_TEXELS, int(thread) / DDGI_IRRADIANCE_TEXELS);
        float3 direction = TexelDirection(irradianceTexel, DDGI_IRRADIANCE_TEXELS);
        float4 sum = float4(0.0);
        float total = 0.0;
        for (int ray = 0; ray < 64; ++ray)
        {
            float4 rayData = sharedRays[ray];
            // Back faces bring no light and do not count: they are what classification looks at.
            if (rayData.w < 0.0)
            {
                continue;
            }
            float weight = max(dot(direction, sharedDirections[ray]), 0.0);
            sum += weight * float4(rayData.rgb, rayData.w >= DDGI_MISS_DISTANCE ? 1.0 : 0.0);
            total += weight;
        }
        float4 value = total > 1e-6 ? sum / total : float4(0.0);
        sharedIrradiance[thread] = value;
        sharedLuminance[thread] = dot(value.rgb, float3(0.2126, 0.7152, 0.0722));
    }
    GroupMemoryBarrierWithGroupSync();

    // Whether the probe's light changed, and the hysteresis this update blends with.
    float4 stats = state.stats;
    if (thread == 0u)
    {
        float luminance = 0.0;
        for (int index = 0; index < 64; ++index)
        {
            luminance += sharedLuminance[index];
        }
        luminance /= 64.0;
        float sigma = sqrt(max(stats.y - stats.x * stats.x, 0.0));
        bool changed = !fresh && (newLighting || (updateCount >= CHANGE_MIN_UPDATES &&
                                                  abs(luminance - stats.x) > CHANGE_SIGMAS * sigma + CHANGE_RELATIVE * stats.x + 1e-6));
        bool restart = false;
        if (changed)
        {
            updateCount = 1;
            restart = luminance > RESTART_RATIO * stats.x || luminance * RESTART_RATIO < stats.x;
        }
        float hysteresis = fresh || restart ? 0.0 : min(constants.hysteresis, float(updateCount) / float(updateCount + 1));
        if (fresh)
        {
            stats.xy = float2(luminance, luminance * luminance);
        }
        else if (changed)
        {
            // A new mean; the spread relative to it carries over.
            float relativeVariance = max(stats.y - stats.x * stats.x, 0.0) / max(stats.x * stats.x, 1e-12);
            stats.xy = float2(luminance, luminance * luminance * (1.0 + relativeVariance));
        }
        else
        {
            stats.xy = lerp(float2(luminance, luminance * luminance), stats.xy, hysteresis);
        }
        sharedHysteresis = hysteresis;
        sharedChanged = changed;
    }
    GroupMemoryBarrierWithGroupSync();
    float hysteresis = sharedHysteresis;

    {
        int3 texel = int3(layerOrigin.xy * (DDGI_VISIBILITY_TEXELS + 2) + 1 + visibilityTexel, layerOrigin.z);
        float2 previous = visibilityImage[texel].rg;
        sharedVisibility[thread] = lerp(sharedVisibility[thread], previous, hysteresis);
    }
    if (thread < 64u)
    {
        int2 irradianceTexel = int2(int(thread) % DDGI_IRRADIANCE_TEXELS, int(thread) / DDGI_IRRADIANCE_TEXELS);
        float3 direction = TexelDirection(irradianceTexel, DDGI_IRRADIANCE_TEXELS);
        float4 value = sharedIrradiance[thread];
        int3 texel = int3(layerOrigin.xy * (DDGI_IRRADIANCE_TEXELS + 2) + 1 + irradianceTexel, layerOrigin.z);
        float4 previous = irradianceImage[texel];
        float keep = hysteresis;
        if (fresh && level + 1u < DdgiLevelCount())
        {
            // A probe with nothing of its own (it just scrolled in, or moved) starts halfway from what
            // the next coarser level sends a surface facing this texel's direction there, so 64 rays do
            // not decide it alone and a moving camera's new probes do not flicker.
            float coverage;
            float3 position = float3(coord) * DdgiLevelSpacing(level) + (sameCoordinate ? state.offset.xyz : float3(0.0));
            float4 coarse = DdgiSampleLevel(level + 1u, position, direction, float3(0.0), coverage);
            previous = coarse;
            keep = 0.5 * clamp(coverage, 0.0, 1.0);
        }
        sharedIrradiance[thread] = lerp(value, previous, keep);
    }
    GroupMemoryBarrierWithGroupSync();

    // Write the interiors and borders from shared memory: 18 x 18 visibility texels, 10 x 10
    // irradiance texels, spread over the 256 invocations.
    const int visibilityTile = DDGI_VISIBILITY_TEXELS + 2;
    for (int index = int(thread); index < visibilityTile * visibilityTile; index += 256)
    {
        int2 tileTexel = int2(index % visibilityTile, index / visibilityTile);
        bool border = tileTexel.x == 0 || tileTexel.y == 0 || tileTexel.x == visibilityTile - 1 || tileTexel.y == visibilityTile - 1;
        int2 source = (border ? BorderSource(tileTexel, DDGI_VISIBILITY_TEXELS) : tileTexel) - 1;
        float2 value = sharedVisibility[source.y * DDGI_VISIBILITY_TEXELS + source.x];
        visibilityImage[int3(layerOrigin.xy * visibilityTile + tileTexel, layerOrigin.z)] = value;
    }
    const int irradianceTile = DDGI_IRRADIANCE_TEXELS + 2;
    if (int(thread) < irradianceTile * irradianceTile)
    {
        int2 tileTexel = int2(int(thread) % irradianceTile, int(thread) / irradianceTile);
        bool border = tileTexel.x == 0 || tileTexel.y == 0 || tileTexel.x == irradianceTile - 1 || tileTexel.y == irradianceTile - 1;
        int2 source = (border ? BorderSource(tileTexel, DDGI_IRRADIANCE_TEXELS) : tileTexel) - 1;
        float4 value = sharedIrradiance[source.y * DDGI_IRRADIANCE_TEXELS + source.x];
        irradianceImage[int3(layerOrigin.xy * irradianceTile + tileTexel, layerOrigin.z)] = value;
    }

    if (thread == 0u)
    {
        float spacing = DdgiLevelSpacing(level);
        float minFrontDistance = 0.2 * spacing;
        int backfaces = 0;
        float closestBack = 1e30;
        float3 closestBackDirection = float3(0.0);
        float closestFront = 1e30;
        float3 closestFrontDirection = float3(0.0);
        float farthestFront = 0.0;
        float3 farthestFrontDirection = float3(0.0);
        for (int ray = 0; ray < 64; ++ray)
        {
            float stored = sharedRays[ray].w;
            if (stored < 0.0)
            {
                // The trace stores a fifth of a back face's distance, negated.
                ++backfaces;
                float distance = -5.0 * stored;
                if (distance < closestBack)
                {
                    closestBack = distance;
                    closestBackDirection = sharedDirections[ray];
                }
                continue;
            }
            float distance = min(stored, 1e4);
            if (distance < closestFront)
            {
                closestFront = distance;
                closestFrontDirection = sharedDirections[ray];
            }
            if (distance > farthestFront)
            {
                farthestFront = distance;
                farthestFrontDirection = sharedDirections[ray];
            }
        }

        float3 previousOffset = sameCoordinate ? state.offset.xyz : float3(0.0);
        float3 offset = previousOffset;
        bool inside = backfaces * 4 > 64;
        if (inside)
        {
            // Toward the open space it sees, not through the nearest back face: a probe on a wall's
            // plane is as near the wall's far side, and crossing it put the probe outside a room,
            // where the sunlit roof above lit the ceiling below. One buried deep sees no front face
            // and stays, inactive, for the probes around it to answer for.
            if (farthestFront > 0.0)
            {
                offset += farthestFrontDirection * min(farthestFront, spacing);
            }
        }
        else if (closestFront < minFrontDistance)
        {
            // Away from the surface, unless the open side is the same way.
            if (dot(closestFrontDirection, farthestFrontDirection) <= 0.0)
            {
                offset += farthestFrontDirection * min(farthestFront, minFrontDistance);
            }
        }
        offset = clamp(offset, float3(-0.45 * spacing), float3(0.45 * spacing));

        // New geometry (the ray scene was rebuilt) judges the probe afresh: what it saw before may be
        // gone.
        bool reclassify = !sameCoordinate || newGeometry;
        float backfaceEvidence = inside || closestBack < spacing ? 1.0 : 0.0;
        float previousEvidence = reclassify ? backfaceEvidence : state.offset.w;
        float evidence = max(backfaceEvidence, 0.9 * previousEvidence);
        int emptyStreak = reclassify ? 0 : (flagsIn >> DDGI_PROBE_EMPTY_STREAK_SHIFT) & DDGI_PROBE_EMPTY_STREAK_MAX;
        emptyStreak = min(closestFront, closestBack) < EMPTY_DISTANCE * spacing ? 0 : min(emptyStreak + 1, DDGI_PROBE_EMPTY_STREAK_MAX);
        int flags = DDGI_PROBE_UPDATED | (min(updateCount + 1, DDGI_PROBE_UPDATE_COUNT_MAX) << DDGI_PROBE_UPDATE_COUNT_SHIFT) |
                    (int(lightingEpoch) << DDGI_PROBE_LIGHTING_EPOCH_SHIFT) | (int(geometryEpoch) << DDGI_PROBE_GEOMETRY_EPOCH_SHIFT) |
                    (emptyStreak << DDGI_PROBE_EMPTY_STREAK_SHIFT);
        uint report = sharedChanged ? DDGI_FEEDBACK_CHANGED : 0u;
        if (evidence > 0.05)
        {
            flags |= DDGI_PROBE_INACTIVE;
            report |= DDGI_FEEDBACK_INACTIVE;
        }
        else if (emptyStreak >= DDGI_PROBE_EMPTY_UPDATES)
        {
            report |= DDGI_FEEDBACK_EMPTY;
        }
        if (length(offset - previousOffset) > 0.05 * spacing)
        {
            flags |= DDGI_PROBE_MOVED;
        }
        DdgiProbeState updated;
        updated.coordAndFlags = int4(coord, flags);
        updated.offset = float4(offset, evidence);
        updated.stats = stats;
        probeStatesOut[stateIndex] = updated;
        uint3 coordBits = uint3(coord) & 0xffu;
        feedback[probe] = report | (coordBits.x << 8) | (coordBits.y << 16) | (coordBits.z << 24);
    }
}
