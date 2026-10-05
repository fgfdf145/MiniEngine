// The cascaded DDGI probe volume (docs/design/2026-09-27-ddgi-design.md) as shaders read it: the
// grid arithmetic of engine/renderer/ddgi_volume.h, the octahedral atlases, and the irradiance a
// surface receives from the probes around it. Include after scene_common.glsl.

#ifndef DDGI_COMMON_GLSL
#define DDGI_COMMON_GLSL

// kDdgiGridSize, kDdgiProbesPerLevel, kDdgiIrradianceTexels and kDdgiVisibilityTexels in
// ddgi_volume.h.
const ivec3 DDGI_GRID = ivec3(32, 16, 32);
const uint DDGI_PROBES_PER_LEVEL = 32u * 16u * 32u;
// kDdgiFadeCellsHorizontal and kDdgiFadeCellsVertical.
const vec3 DDGI_FADE_CELLS = vec3(3.0, 2.0, 3.0);
const int DDGI_IRRADIANCE_TEXELS = 8;
const int DDGI_VISIBILITY_TEXELS = 16;

// DdgiProbeState flags (w of coordAndFlags).
const int DDGI_PROBE_UPDATED = 1;
// Inside geometry (too many of its rays hit back faces): not sampled, still traced, so an object that
// moves away revives it.
const int DDGI_PROBE_INACTIVE = 2;
// Relocated by more than a twentieth of the spacing at its last update: its tiles describe the old
// position, so the next update keeps none of them.
const int DDGI_PROBE_MOVED = 4;
// Bits 8 to 15: how many updates the probe has had since it was last fresh or its light last
// changed (saturating at 255).
const int DDGI_PROBE_UPDATE_COUNT_SHIFT = 8;
const int DDGI_PROBE_UPDATE_COUNT_MAX = 255;
// Bits 16 to 23: the lighting epoch of its last update (DdgiLightingWatch); bits 24 to 27: the
// geometry epoch (the ray scene's installs), mod 16; bits 28 to 30: how many updates in a row saw no
// surface near enough to be lit by it (DDGI_PROBE_EMPTY_UPDATES of them make it empty).
const int DDGI_PROBE_LIGHTING_EPOCH_SHIFT = 16;
const int DDGI_PROBE_GEOMETRY_EPOCH_SHIFT = 24;
const int DDGI_PROBE_EMPTY_STREAK_SHIFT = 28;
const int DDGI_PROBE_EMPTY_STREAK_MAX = 7;
const int DDGI_PROBE_EMPTY_UPDATES = 3;

// One probe's record: the world grid coordinate whose data it holds and flags, its relocation offset
// in metres (xyz) and its smoothed back-face evidence (w, ddgi_update.comp), and the running mean and
// mean square of its updates' average luminance (stats.xy), which tell a change in its light from
// noise. A probe is sampled only where the coordinate matches the one its slot should hold now: after
// a scroll, a slot keeps the old place's data until it is updated. kDdgiProbeStateBytes in
// ddgi_volume.h.
struct DdgiProbeState
{
    ivec4 coordAndFlags;
    vec4 offset;
    vec4 stats;
};

// What the update reports to the CPU per scheduled probe (DdgiProbeScheduler::ApplyFeedback): these
// bits, and the probe's coordinate mod 256 per axis above bit 8.
const uint DDGI_FEEDBACK_CHANGED = 1u;
const uint DDGI_FEEDBACK_EMPTY = 2u;
const uint DDGI_FEEDBACK_INACTIVE = 4u;

// Set 0 bindings 21 to 23 (VulkanDdgi): one array layer per level, a 10 x 10 (irradiance: rgb
// irradiance / pi, a sky visibility) or 18 x 18 (visibility: mean distance, mean squared distance)
// tile per probe, bordered.
layout(set = 0, binding = 21) uniform sampler2DArray ddgiIrradianceAtlas;
layout(set = 0, binding = 22) uniform sampler2DArray ddgiVisibilityAtlas;
layout(set = 0, binding = 23, std430) readonly buffer DdgiProbeStates
{
    DdgiProbeState ddgiProbeStates[];
};

uint DdgiLevelCount()
{
    return uint(ubo.ddgiParams.x);
}

// coord mod the grid size, non-negative (GLSL's % is undefined for negative operands).
ivec3 DdgiStorageSlot(ivec3 coord)
{
    vec3 value = vec3(coord);
    vec3 size = vec3(DDGI_GRID);
    return ivec3(value - size * floor(value / size));
}

ivec3 DdgiSlotCoordinate(ivec3 slot, ivec3 origin)
{
    return origin + DdgiStorageSlot(slot - origin);
}

uint DdgiSlotIndex(ivec3 slot)
{
    return uint(slot.x + DDGI_GRID.x * (slot.z + DDGI_GRID.z * slot.y));
}

ivec3 DdgiSlotFromIndex(uint index)
{
    int value = int(index);
    return ivec3(value % DDGI_GRID.x, value / (DDGI_GRID.x * DDGI_GRID.z), (value / DDGI_GRID.x) % DDGI_GRID.z);
}

// A probe's tile in its level's atlas layer: x + grid.x * y across, z down.
ivec2 DdgiTile(ivec3 slot)
{
    return ivec2(slot.x + DDGI_GRID.x * slot.y, slot.z);
}

ivec3 DdgiLevelOrigin(uint level)
{
    return ivec3(ubo.ddgiOrigins[level].xyz);
}

float DdgiLevelSpacing(uint level)
{
    return ubo.ddgiSpacing[level];
}

vec2 DdgiSignNotZero(vec2 value)
{
    return vec2(value.x >= 0.0 ? 1.0 : -1.0, value.y >= 0.0 ? 1.0 : -1.0);
}

// A unit direction to [-1, 1]^2: the octahedron around +Y unfolded, the lower half folded out.
vec2 DdgiOctEncode(vec3 direction)
{
    vec3 folded = direction / (abs(direction.x) + abs(direction.y) + abs(direction.z));
    vec2 encoded = folded.xz;
    if (folded.y < 0.0)
    {
        encoded = (1.0 - abs(encoded.yx)) * DdgiSignNotZero(encoded);
    }
    return encoded;
}

vec3 DdgiOctDecode(vec2 encoded)
{
    vec3 direction = vec3(encoded.x, 1.0 - abs(encoded.x) - abs(encoded.y), encoded.y);
    if (direction.y < 0.0)
    {
        direction.xz = (1.0 - abs(direction.zx)) * DdgiSignNotZero(direction.xz);
    }
    return normalize(direction);
}

// Where a direction samples a probe's tile, in texture coordinates of a layer: inside the border.
vec2 DdgiAtlasUv(ivec3 slot, vec3 direction, int texels, vec2 atlasSize)
{
    vec2 tileCorner = vec2(DdgiTile(slot) * (texels + 2) + 1);
    vec2 inside = (DdgiOctEncode(direction) * 0.5 + 0.5) * float(texels);
    return (tileCorner + inside) / atlasSize;
}

// The probe's world position: its grid point plus its relocation, when its record is for this
// coordinate.
vec3 DdgiProbePosition(uint level, ivec3 coord, DdgiProbeState state)
{
    vec3 position = vec3(coord) * DdgiLevelSpacing(level);
    return state.coordAndFlags.xyz == coord ? position + state.offset.xyz : position;
}

// What one level's probes send to a surface at P facing N, seen from the direction V (toward the
// viewer): rgb irradiance / pi, a sky visibility, blended over the eight probes around the point by
// trilinear weight, a wrap term that favours probes in front of the surface, and the Chebyshev test
// on the probe's distance moments that drops probes behind walls (Majercik et al. 2019), read along D:
// N for the irradiance the surface receives, another direction for what arrives from there (a
// specular lobe's). The probes are chosen for the surface either way. coverage is
// the trilinear weight of the probes updated for where they are, inactive ones included: 0 outside
// the grid or where no probe around can be used.
vec4 DdgiSampleLevelAlong(uint level, vec3 P, vec3 N, vec3 V, vec3 D, out float coverage)
{
    coverage = 0.0;
    float spacing = DdgiLevelSpacing(level);
    ivec3 origin = DdgiLevelOrigin(level);
    vec3 biased = P + (N * ubo.ddgiParams.z + V * ubo.ddgiParams.w) * spacing;
    vec3 gridPosition = biased / spacing - vec3(origin);
    ivec3 base = ivec3(floor(gridPosition));
    if (any(lessThan(base, ivec3(0))) || any(greaterThan(base + 1, DDGI_GRID - 1)))
    {
        return vec4(0.0);
    }
    vec3 alpha = gridPosition - vec3(base);
    vec2 irradianceSize = vec2(textureSize(ddgiIrradianceAtlas, 0).xy);
    vec2 visibilitySize = vec2(textureSize(ddgiVisibilityAtlas, 0).xy);

    vec4 sum = vec4(0.0);
    float totalWeight = 0.0;
    for (int corner = 0; corner < 8; ++corner)
    {
        ivec3 offset = ivec3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
        ivec3 coord = origin + base + offset;
        ivec3 slot = DdgiStorageSlot(coord);
        DdgiProbeState state = ddgiProbeStates[level * DDGI_PROBES_PER_LEVEL + DdgiSlotIndex(slot)];
        if (state.coordAndFlags.xyz != coord || (state.coordAndFlags.w & DDGI_PROBE_UPDATED) == 0)
        {
            continue;
        }
        vec3 trilinear3 = mix(1.0 - alpha, alpha, vec3(offset));
        float trilinear = trilinear3.x * trilinear3.y * trilinear3.z;
        // A probe inside geometry is known, not missing: the probes around it answer for its share.
        // Counting it out of the coverage would hand that share to the coarser levels and the
        // unoccluded sky, which light a wall that has probes buried in it as if it stood in the open.
        coverage += trilinear;
        if ((state.coordAndFlags.w & DDGI_PROBE_INACTIVE) != 0)
        {
            continue;
        }
        vec3 probePosition = vec3(coord) * spacing + state.offset.xyz;

        vec3 toProbe = normalize(probePosition - P);
        float weight = (dot(toProbe, N) + 1.0) * 0.5;
        weight = weight * weight + 0.2;

        vec3 probeToPoint = biased - probePosition;
        float distance = length(probeToPoint);
        vec2 moments = textureLod(
                           ddgiVisibilityAtlas,
                           vec3(DdgiAtlasUv(slot, probeToPoint / max(distance, 1e-4), DDGI_VISIBILITY_TEXELS, visibilitySize), float(level)),
                           0.0)
                           .rg;
        float chebyshev = 1.0;
        if (distance > moments.x)
        {
            // Capped: near a building's edge a probe's lobe mixes rays stopped by a floor with rays that
            // slipped past it, and the spread would let the probe light the rooms above through the floor.
            float maxDeviation = 0.05 * spacing;
            float variance = min(abs(moments.x * moments.x - moments.y), maxDeviation * maxDeviation);
            float excess = distance - moments.x;
            chebyshev = variance / (variance + excess * excess);
            chebyshev = chebyshev * chebyshev * chebyshev;
        }
        weight *= max(chebyshev, 0.05);
        weight = max(weight, 1e-6);
        // Crush tiny weights so a barely visible probe cannot tint the result.
        const float crush = 0.2;
        if (weight < crush)
        {
            weight *= weight * weight / (crush * crush);
        }
        weight *= trilinear;

        vec4 irradiance = textureLod(
            ddgiIrradianceAtlas,
            vec3(DdgiAtlasUv(slot, D, DDGI_IRRADIANCE_TEXELS, irradianceSize), float(level)),
            0.0);
        sum += irradiance * weight;
        totalWeight += weight;
    }
    if (totalWeight <= 0.0)
    {
        // Every probe around is inside geometry or stale: this level knows nothing here.
        coverage = 0.0;
        return vec4(0.0);
    }
    return sum / totalWeight;
}

vec4 DdgiSampleLevel(uint level, vec3 P, vec3 N, vec3 V, out float coverage)
{
    return DdgiSampleLevelAlong(level, P, N, V, N, coverage);
}

// The volume's irradiance / pi (rgb) and sky visibility (a) around D at P, on a surface facing N, seen
// from V (DdgiIrradiance: around N itself): the finest
// level holding the point, fading into the next over its outermost cells. weight is how much of the
// answer the volume gives, 1 inside it, 0 outside every level; the caller supplies the rest.
vec4 DdgiIrradianceAlong(vec3 P, vec3 N, vec3 V, vec3 D, out float weight)
{
    vec4 result = vec4(0.0);
    float remaining = 1.0;
    uint levelCount = DdgiLevelCount();
    for (uint level = 0u; level < levelCount && remaining > 1e-3; ++level)
    {
        // The level answers within half its grid minus one cell of the camera (ComputeDdgiLevel in
        // ddgi_volume.cpp), less half a cell for the lookup's bias, fading over its outer cells
        // (DDGI_FADE_CELLS): three across, so the light a surface gets changes gradually as the camera
        // walks toward it, two up and down, where the grid is shallower and a wider band handed much
        // of a room's ceiling to levels four and eight times coarser. By the distance to the camera,
        // not to the grid's faces: those jump a cell when the grid scrolls, and the whole image
        // changed with them.
        vec3 fromCamera = abs(P - ubo.cameraWorldPosition.xyz) / DdgiLevelSpacing(level);
        vec3 toEdge = (vec3(DDGI_GRID / 2 - 1) - 0.5 - fromCamera) / DDGI_FADE_CELLS;
        float fade = clamp(min(min(toEdge.x, toEdge.y), toEdge.z), 0.0, 1.0);
        if (fade <= 0.0)
        {
            continue;
        }
        float coverage;
        vec4 value = DdgiSampleLevelAlong(level, P, N, V, D, coverage);
        float levelWeight = fade * clamp(coverage, 0.0, 1.0);
        result += value * levelWeight * remaining;
        remaining *= 1.0 - levelWeight;
    }
    weight = 1.0 - remaining;
    return result;
}

vec4 DdgiIrradiance(vec3 P, vec3 N, vec3 V, out float weight)
{
    return DdgiIrradianceAlong(P, N, V, N, weight);
}

#endif
