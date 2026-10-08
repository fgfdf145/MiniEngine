// The emissive triangles as lights (VulkanPathTraceLights, docs/design/
// 2026-10-08-path-tracing-remaining-work-design.md): every triangle of the installed submeshes whose
// material emits, in world space this frame, and a tree of their powers that next event estimation
// picks one from in proportion to its power. Rebuilt on the GPU every frame by emissive_lights.comp,
// which the path tracer's trace then reads. The set is EMISSIVE_LIGHT_SET (4 in the trace); the
// building shader defines EMISSIVE_LIGHTS_WRITE.
//
// Include after scene_common.slang, ray_tracing_common.slang, material_common.slang, material_uv.slang and
// ray_hit_common.slang.

#ifndef EMISSIVE_LIGHTS_COMMON_SLANG
#define EMISSIVE_LIGHTS_COMMON_SLANG

#ifndef EMISSIVE_LIGHT_SET
#define EMISSIVE_LIGHT_SET 4
#endif

#ifdef EMISSIVE_LIGHTS_WRITE
#define EmissiveBuffer RWStructuredBuffer
#else
#define EmissiveBuffer StructuredBuffer
#endif

// A light: its triangle in world space (v0, and the edges to v1 and v2), which side emits (+1 the side
// cross(e1, e2) points to, -1 the other, 0 both: a double-sided material), and what a sample needs of
// its material, so it reads nothing else but the emission map: its draw slot, the map's coordinates
// at the corners (through the material's texture transform, which is affine), the map's level of
// detail for a footprint one unit across seen face on, and the emissive factor.
struct EmissiveTriangle
{
    float4 v0Lod;     // xyz v0, w the level of detail
    float4 e1Slot;    // xyz e1, w draw slot as uint bits
    float4 e2Front;   // xyz e2, w the emitting side
    float4 uv01;      // xy at v0, zw at v1
    float4 uv2;       // xy at v2
    float4 emission;  // rgb the emissive factor
};

[[vk::binding(0, EMISSIVE_LIGHT_SET)]] EmissiveBuffer<EmissiveTriangle> emissiveTriangles;
// The power tree, four children a node: element 0 holds the root (the total power) in x, the depth in
// y and the light count in z (uint bits); level k (1 to depth, 4^k nodes) starts at float
// EmissiveTreeLevelStart(k), so each node's four children are one float4. The leaves are the lights' powers
// (0 past the count).
[[vk::binding(1, EMISSIVE_LIGHT_SET)]] EmissiveBuffer<float4> emissiveTree;
// By draw slot: the slot's first light (its triangle k is light first + k), ~0 when it emits nothing.
[[vk::binding(2, EMISSIVE_LIGHT_SET)]] StructuredBuffer<uint> emissiveSlotBase;
// By draw slot: the instance that draws it this frame, ~0 when none does (emissive_lights.comp).
[[vk::binding(3, EMISSIVE_LIGHT_SET)]] EmissiveBuffer<uint> emissiveSlotInstance;
// By light: its draw slot and its triangle's index in the slot's mesh.
[[vk::binding(4, EMISSIVE_LIGHT_SET)]] StructuredBuffer<uint2> emissiveEntries;

uint EmissiveTreeLevelStart(uint level)
{
    // 4 + 4 + 16 + ... + 4^(level - 1) floats come before it: the header and the levels above.
    return 4u + ((1u << (2u * level)) - 4u) / 3u;
}

float EmissiveTreeValue(uint index)
{
    return emissiveTree[index >> 2u][index & 3u];
}

#ifndef EMISSIVE_LIGHTS_WRITE
float EmissiveTotalPower()
{
    return emissiveTree[0].x;
}

// The probability next event estimation picks the light (of its triangle as a whole).
float EmissiveLightProbability(uint light)
{
    uint depth = asuint(emissiveTree[0].y);
    float total = emissiveTree[0].x;
    return total > 0.0 ? EmissiveTreeValue(EmissiveTreeLevelStart(depth) + light) / total : 0.0;
}

// The light of a ray's hit, ~0 when it is none (the draw slot emits nothing, or is not in the list).
uint EmissiveLightOfHit(RayHit hit)
{
    RayInstance instance = rayInstances[hit.instance];
    uint slot = instance.data.z;
    if (slot >= BufferLength(emissiveSlotBase))
    {
        return ~0u;
    }
    uint base = emissiveSlotBase[slot];
    return base == ~0u ? ~0u : base + (hit.triangle - instance.data.y);
}

// A light picked in proportion to its power: one walk down the tree, u in [0, 1) reused at each
// level. False when there is nothing to pick.
bool PickEmissiveLight(float u, out uint light)
{
    light = 0u;
    if (emissiveTree[0].x <= 0.0)
    {
        return false;
    }
    uint depth = asuint(emissiveTree[0].y);
    uint node = 0u;
    for (uint level = 1u; level <= depth; ++level)
    {
        float4 children = emissiveTree[(EmissiveTreeLevelStart(level) >> 2u) + node];
        float sum = children.x + children.y + children.z + children.w;
        if (!(sum > 0.0))
        {
            return false;
        }
        float target = u * sum;
        uint pick = 3u;
        float before = 0.0;
        for (uint child = 0u; child < 3u; ++child)
        {
            if (target < before + children[child])
            {
                pick = child;
                break;
            }
            before += children[child];
        }
        float weight = children[pick];
        if (!(weight > 0.0))
        {
            return false;
        }
        u = clamp((target - before) / weight, 0.0, 0.99999994);
        node = node * 4u + pick;
    }
    light = node;
    return light < asuint(emissiveTree[0].z);
}

// A point on a light: where, the emitting side's normal facing the receiver (zero when the receiver
// is behind a one-sided light), the light it sends toward the receiver (its material's emission, the
// map at the level of detail a footprint of footprintWidth wants) and the probability density of
// picking that point per unit area.
struct EmissivePoint
{
    float3 position;
    float3 normal;
    float3 radiance;
    float areaDensity;
};

bool SampleEmissivePoint(uint light, float2 u, float3 receiver, float footprintWidth, out EmissivePoint point)
{
    point.position = float3(0.0);
    point.normal = float3(0.0);
    point.radiance = float3(0.0);
    point.areaDensity = 0.0;
    EmissiveTriangle triangle = emissiveTriangles[light];
    float3 crossed = cross(triangle.e1Slot.xyz, triangle.e2Front.xyz);
    float doubleArea = length(crossed);
    if (doubleArea <= 0.0)
    {
        return false;
    }
    // Uniform over the triangle.
    float root = sqrt(u.x);
    float2 barycentrics = float2(root * (1.0 - u.y), root * u.y);
    point.position = triangle.v0Lod.xyz + triangle.e1Slot.xyz * barycentrics.x + triangle.e2Front.xyz * barycentrics.y;
    float3 normal = crossed / doubleArea;
    float3 toReceiver = receiver - point.position;
    float side = triangle.e2Front.w;
    float facing = dot(normal, toReceiver);
    if (side != 0.0 && facing * side <= 0.0)
    {
        return false;
    }
    point.normal = facing >= 0.0 ? normal : -normal;
    point.areaDensity = EmissiveLightProbability(light) * 2.0 / doubleArea;

    // The emission map where the point is, at RayTextureLod's level for the footprint.
    uint textureIndex = asuint(triangle.e1Slot.w) * RAY_TEXTURES_PER_SLOT + RAY_TEXTURE_EMISSIVE;
    float2 uv = triangle.uv01.xy * (1.0 - barycentrics.x - barycentrics.y) + triangle.uv01.zw * barycentrics.x + triangle.uv2.xy * barycentrics.y;
    float cosTheta = abs(facing) / max(length(toReceiver), 1e-6);
    float lod = triangle.v0Lod.w + log2(max(footprintWidth, 1e-6) / max(cosTheta, 0.05));
    point.radiance = rayTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(uv, lod).rgb * triangle.emission.rgb;
    return point.areaDensity > 0.0;
}
#endif

#endif
