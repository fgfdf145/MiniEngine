// The ray scene (VulkanRayScene, engine/renderer/vulkan/ray_scene.h) and its traversal: the same
// arrays and the same walk as TraceRay in engine/renderer/ray_tracing_bvh.cpp. A shader that includes
// this binds the ray set at RAY_SCENE_SET (1 unless defined before the include).
//
// Compiled with RAY_QUERY defined (the *_ray_query.comp.spv variants, Vulkan 1.2 SPIR-V), TraceSceneRay
// asks the hardware instead: a ray query against the top-level acceleration structure at binding 5,
// whose instances are the same RayInstances in the same order (VulkanRayAcceleration). The hit it
// reports names the same instance and triangle, so everything after it reads the same arrays.

#ifndef RAY_TRACING_COMMON_SLANG
#define RAY_TRACING_COMMON_SLANG

#ifndef RAY_SCENE_SET
#define RAY_SCENE_SET 1
#endif

// kMaxBvhDepth + 1 in ray_tracing_bvh.h covers one level; the two levels share this stack, and the
// top level of a few thousand instances is some 15 deep. A full stack drops the farthest nodes
// rather than overflow.
#define RAY_STACK_SIZE 48

// Instance flags (RayInstance::data.w's low bits), matching kRayInstance* in ray_tracing_bvh.h; the
// instance's mesh index sits above RAY_INSTANCE_MESH_SHIFT.
#define RAY_INSTANCE_SKIP 1u
#define RAY_INSTANCE_DYNAMIC 2u
#define RAY_INSTANCE_NO_SHADOW 4u
#define RAY_INSTANCE_BLEND 8u
#define RAY_INSTANCE_MESH_SHIFT 4u

// Which instances a ray sees, matching kRayMask* in ray_acceleration.h (the top level's instance
// masks, one bit each for static and moving instances that do and do not cast shadows, and one for
// the Blend surfaces). The probes' rays leave moving instances out, so a passing car leaves no trail in
// their light; the per-pixel visibility rays (occlusion, reflections) see everything traceable but the
// Blend surfaces (decals, glass), which only the path tracer's rays meet. Shadow rays, towards a light,
// leave out what casts no shadow (a track's ground and grass, as its model says).
#define RAY_MASK_STATIC_CASTER 1u
#define RAY_MASK_DYNAMIC_CASTER 2u
#define RAY_MASK_STATIC_NO_SHADOW 4u
#define RAY_MASK_DYNAMIC_NO_SHADOW 8u
#define RAY_MASK_BLEND 16u
#define RAY_MASK_DYNAMIC (RAY_MASK_DYNAMIC_CASTER | RAY_MASK_DYNAMIC_NO_SHADOW)
#define RAY_MASK_NO_SHADOW (RAY_MASK_STATIC_NO_SHADOW | RAY_MASK_DYNAMIC_NO_SHADOW)
#define RAY_MASK_PROBE (RAY_MASK_STATIC_CASTER | RAY_MASK_STATIC_NO_SHADOW)
#define RAY_MASK_VISIBILITY (RAY_MASK_STATIC_CASTER | RAY_MASK_DYNAMIC_CASTER | RAY_MASK_NO_SHADOW)
#define RAY_MASK_SHADOW (RAY_MASK_STATIC_CASTER | RAY_MASK_DYNAMIC_CASTER)
#define RAY_MASK_PROBE_SHADOW RAY_MASK_STATIC_CASTER
#define RAY_MASK_PATH (RAY_MASK_VISIBILITY | RAY_MASK_BLEND)

// Ray material flags (RayMaterial.emissionFlags.w as uint bits), matching ray_scene.cpp.
#define RAY_MATERIAL_DOUBLE_SIDED 1u
#define RAY_MATERIAL_ALPHA_MASK 2u
#define RAY_MATERIAL_ALPHA_BLEND 4u
#define RAY_MATERIAL_TRANSMISSION 8u
#define RAY_MATERIAL_NORMAL_MAP 16u

struct BvhNode
{
    float4 boundsMinFirst; // xyz min, w first as uint bits
    float4 boundsMaxCount; // xyz max, w count as uint bits (0: inner node)
};

struct BvhTriangle
{
    float4 v0;
    float4 e1;
    float4 e2;
};

struct RayInstance
{
    float4 worldToObject[3];
    uint4 data; // x node offset, y triangle offset, z ray material, w flags
};

// The low-frequency material a probe ray sees (ray_material_average.comp).
struct RayMaterial
{
    float4 albedoCoverage; // rgb average albedo, a the share of the surface that stops rays
    float4 emissionFlags;  // rgb average emission (cd/m^2), w flags as uint bits
};

[[vk::binding(0, RAY_SCENE_SET)]] StructuredBuffer<BvhNode> meshNodes;
[[vk::binding(1, RAY_SCENE_SET)]] StructuredBuffer<BvhTriangle> meshTriangles;
[[vk::binding(2, RAY_SCENE_SET)]] StructuredBuffer<RayInstance> rayInstances;
[[vk::binding(3, RAY_SCENE_SET)]] StructuredBuffer<BvhNode> topNodes;
[[vk::binding(4, RAY_SCENE_SET)]] StructuredBuffer<RayMaterial> rayMaterials;

#ifdef RAY_QUERY
[[vk::binding(5, RAY_SCENE_SET)]] RaytracingAccelerationStructure rayTopLevel;
#endif

struct RayHit
{
    float t;
    uint instance;
    uint triangle;
    float2 barycentrics;
    bool frontFace;
};

// 1 / direction, with components too small to invert replaced by +-1e-20 (sign kept, zero positive).
float3 SafeInverse(float3 direction)
{
    const float tiny = 1e-20;
    float3 floorValue = float3(tiny) * (step(0.0, direction) * 2.0 - 1.0);
    return 1.0 / select(abs(direction) > float3(tiny), direction, floorValue);
}

// The entry distance when the ray meets the box within [tMin, tMax], else a negative number.
float IntersectBounds(float3 boundsMin, float3 boundsMax, float3 origin, float3 inverseDirection, float tMin, float tMax)
{
    float3 t0 = (boundsMin - origin) * inverseDirection;
    float3 t1 = (boundsMax - origin) * inverseDirection;
    float3 near = min(t0, t1);
    float3 far = max(t0, t1);
    float enter = max(max(near.x, near.y), max(near.z, tMin));
    float exit = min(min(far.x, far.y), min(far.z, tMax));
    return enter <= exit ? enter : -1.0;
}

// Moller-Trumbore; frontFace when the triangle's counter-clockwise side faces the ray.
bool IntersectTriangle(BvhTriangle triangle, float3 origin, float3 direction, float tMin, float tMax, out float t, out float2 barycentrics, out bool frontFace)
{
    t = 0.0;
    barycentrics = float2(0.0);
    frontFace = false;
    float3 e1 = triangle.e1.xyz;
    float3 e2 = triangle.e2.xyz;
    float3 p = cross(direction, e2);
    float determinant = dot(e1, p);
    if (determinant == 0.0)
    {
        return false;
    }
    float inverseDeterminant = 1.0 / determinant;
    float3 s = origin - triangle.v0.xyz;
    float u = dot(s, p) * inverseDeterminant;
    if (u < 0.0 || u > 1.0)
    {
        return false;
    }
    float3 q = cross(s, e1);
    float v = dot(direction, q) * inverseDeterminant;
    if (v < 0.0 || u + v > 1.0)
    {
        return false;
    }
    t = dot(e2, q) * inverseDeterminant;
    if (!(t > tMin && t < tMax))
    {
        return false;
    }
    barycentrics = float2(u, v);
    frontFace = determinant > 0.0;
    return true;
}

// A hash in [0, 1) of two integers (PCG), for the stochastic coverage of Mask and clear surfaces.
float RayHash(uint a, uint b)
{
    uint state = a * 747796405u + 2891336453u + b * 277803737u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    word = (word >> 22u) ^ word;
    return float(word) * (1.0 / 4294967296.0);
}

// Set by the path tracer for its own rays, which refract through a transmissive surface themselves:
// they meet it whole, where every other ray passes through the share its transmission says.
static bool rayMeetsTransmission = false;

#ifdef RAY_TEXTURED_ALPHA
// In ray_hit_common.slang, which a shader defining RAY_TEXTURED_ALPHA includes after this file.
bool AcceptTexturedHit(uint instance, uint triangle, float2 barycentrics, uint rayId);
// Cleared by a shader for rays whose result is averaged over a wide lobe (the path tracer's diffuse
// bounces), where the coverage decision gives the same light for less: the texture test is the most
// expensive part of tracing through foliage.
static bool rayTexturedAlpha = true;
#endif

// Whether a candidate hit stops the ray: a partly covered surface (foliage cards, glass) stops the
// share of rays its coverage says, decided per ray and triangle so a ray agrees with itself. With
// RAY_TEXTURED_ALPHA an alpha-tested surface reads its texture instead (AcceptTexturedHit).
bool AcceptHit(uint instance, uint triangle, float2 barycentrics, uint rayId)
{
    if (rayMeetsTransmission &&
        (asuint(rayMaterials[rayInstances[instance].data.z].emissionFlags.w) & RAY_MATERIAL_TRANSMISSION) != 0u)
    {
        return true;
    }
#ifdef RAY_TEXTURED_ALPHA
    if (rayTexturedAlpha)
    {
        return AcceptTexturedHit(instance, triangle, barycentrics, rayId);
    }
#endif
    float coverage = rayMaterials[rayInstances[instance].data.z].albedoCoverage.a;
    return coverage >= 1.0 || RayHash(rayId, triangle) < coverage;
}

#ifdef RAY_QUERY
// The nearest accepted hit within (tMin, tMax), or the first found when anyHit (shadow rays), among
// the instances rayMask names (RAY_MASK_*). rayId seeds the coverage decisions. Instances whose
// material stops every ray are opaque to the hardware (VulkanRayAcceleration::UpdateTopLevel); the
// others' candidates come back here for AcceptHit.
bool TraceSceneRayMasked(float3 origin, float3 direction, float tMin, float tMax, bool anyHit, uint rayId, uint rayMask, out RayHit hit)
{
    hit.t = tMax;
    hit.instance = 0u;
    hit.triangle = 0u;
    hit.barycentrics = float2(0.0);
    hit.frontFace = false;

    RayQuery<RAY_FLAG_NONE> query;
    RayDesc ray;
    ray.Origin = origin;
    ray.TMin = tMin;
    ray.Direction = direction;
    ray.TMax = tMax;
    query.TraceRayInline(rayTopLevel, anyHit ? RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH : RAY_FLAG_NONE, rayMask, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            continue;
        }
        uint instance = query.CandidateInstanceID();
        uint triangle = rayInstances[instance].data.y + query.CandidatePrimitiveIndex();
        if (AcceptHit(instance, triangle, query.CandidateTriangleBarycentrics(), rayId))
        {
            query.CommitNonOpaqueTriangleHit();
        }
    }
    if (query.CommittedStatus() == COMMITTED_NOTHING)
    {
        return false;
    }
    hit.t = query.CommittedRayT();
    hit.instance = query.CommittedInstanceID();
    hit.triangle = rayInstances[hit.instance].data.y + query.CommittedPrimitiveIndex();
    hit.barycentrics = query.CommittedTriangleBarycentrics();
    // The compute walk's rule (IntersectTriangle's determinant) from the triangle itself, rather than
    // the API's facing convention: the counter-clockwise side faces a ray running against its normal.
    BvhTriangle triangle = meshTriangles[hit.triangle];
    float3 objectDirection = query.CommittedObjectRayDirection();
    hit.frontFace = dot(objectDirection, cross(triangle.e1.xyz, triangle.e2.xyz)) < 0.0;
    return true;
}
#else
// The nearest accepted hit within (tMin, tMax), or the first found when anyHit (shadow rays). rayId
// seeds the coverage decisions. Both hierarchy levels share one stack: an instance's nodes are pushed
// over the top level's and all popped before the next top-level entry, so the current instance's
// transformed ray is the right one for every mesh entry popped. rayMask as for the ray query variant.
bool TraceSceneRayMasked(float3 origin, float3 direction, float tMin, float tMax, bool anyHit, uint rayId, uint rayMask, out RayHit hit)
{
    uint skipFlags = RAY_INSTANCE_SKIP | ((rayMask & RAY_MASK_DYNAMIC) != 0u ? 0u : RAY_INSTANCE_DYNAMIC) |
                     ((rayMask & RAY_MASK_NO_SHADOW) != 0u ? 0u : RAY_INSTANCE_NO_SHADOW) |
                     ((rayMask & RAY_MASK_BLEND) != 0u ? 0u : RAY_INSTANCE_BLEND);
    hit.t = tMax;
    hit.instance = 0u;
    hit.triangle = 0u;
    hit.barycentrics = float2(0.0);
    hit.frontFace = false;
    bool found = false;

    const uint MESH_ENTRY = 0x80000000u;
    uint stack[RAY_STACK_SIZE];
    uint size = 0u;
    stack[size++] = 0u;

    float3 worldInverse = SafeInverse(direction);
    uint instanceIndex = 0u;
    float3 localOrigin = origin;
    float3 localDirection = direction;
    float3 localInverse = worldInverse;
    uint nodeOffset = 0u;
    uint triangleOffset = 0u;

    while (size > 0u)
    {
        uint entry = stack[--size];
        if ((entry & MESH_ENTRY) == 0u)
        {
            BvhNode node = topNodes[entry];
            if (IntersectBounds(node.boundsMinFirst.xyz, node.boundsMaxCount.xyz, origin, worldInverse, tMin, hit.t) < 0.0)
            {
                continue;
            }
            uint first = asuint(node.boundsMinFirst.w);
            uint count = asuint(node.boundsMaxCount.w);
            if (count == 0u)
            {
                if (size + 2u <= RAY_STACK_SIZE)
                {
                    stack[size++] = first + 1u;
                    stack[size++] = first;
                }
                continue;
            }
            // One instance per top-level leaf (BuildTopLevel).
            RayInstance instance = rayInstances[first];
            if ((instance.data.w & skipFlags) != 0u)
            {
                continue;
            }
            instanceIndex = first;
            localOrigin = float3(dot(instance.worldToObject[0], float4(origin, 1.0)), dot(instance.worldToObject[1], float4(origin, 1.0)), dot(instance.worldToObject[2], float4(origin, 1.0)));
            localDirection = float3(dot(instance.worldToObject[0].xyz, direction), dot(instance.worldToObject[1].xyz, direction), dot(instance.worldToObject[2].xyz, direction));
            localInverse = SafeInverse(localDirection);
            nodeOffset = instance.data.x;
            triangleOffset = instance.data.y;
            if (size < RAY_STACK_SIZE)
            {
                stack[size++] = MESH_ENTRY;
            }
            continue;
        }

        BvhNode node = meshNodes[nodeOffset + (entry & ~MESH_ENTRY)];
        uint first = asuint(node.boundsMinFirst.w);
        uint count = asuint(node.boundsMaxCount.w);
        if (count == 0u)
        {
            BvhNode left = meshNodes[nodeOffset + first];
            BvhNode right = meshNodes[nodeOffset + first + 1u];
            float leftEnter = IntersectBounds(left.boundsMinFirst.xyz, left.boundsMaxCount.xyz, localOrigin, localInverse, tMin, hit.t);
            float rightEnter = IntersectBounds(right.boundsMinFirst.xyz, right.boundsMaxCount.xyz, localOrigin, localInverse, tMin, hit.t);
            bool hitLeft = leftEnter >= 0.0;
            bool hitRight = rightEnter >= 0.0;
            if (hitLeft && hitRight)
            {
                bool leftFirst = leftEnter <= rightEnter;
                if (size + 2u <= RAY_STACK_SIZE)
                {
                    stack[size++] = MESH_ENTRY | (first + (leftFirst ? 1u : 0u));
                    stack[size++] = MESH_ENTRY | (first + (leftFirst ? 0u : 1u));
                }
            }
            else if ((hitLeft || hitRight) && size < RAY_STACK_SIZE)
            {
                stack[size++] = MESH_ENTRY | (first + (hitLeft ? 0u : 1u));
            }
            continue;
        }
        for (uint k = first; k < first + count; ++k)
        {
            float t;
            float2 barycentrics;
            bool frontFace;
            if (!IntersectTriangle(meshTriangles[triangleOffset + k], localOrigin, localDirection, tMin, hit.t, t, barycentrics, frontFace))
            {
                continue;
            }
            if (!AcceptHit(instanceIndex, triangleOffset + k, barycentrics, rayId))
            {
                continue;
            }
            hit.t = t;
            hit.instance = instanceIndex;
            hit.triangle = triangleOffset + k;
            hit.barycentrics = barycentrics;
            hit.frontFace = frontFace;
            found = true;
            if (anyHit)
            {
                return true;
            }
        }
    }
    return found;
}
#endif

// What the probes trace: moving instances left out.
bool TraceSceneRay(float3 origin, float3 direction, float tMin, float tMax, bool anyHit, uint rayId, out RayHit hit)
{
    return TraceSceneRayMasked(origin, direction, tMin, tMax, anyHit, rayId, RAY_MASK_PROBE, hit);
}

// A probe's shadow ray, towards a light: its static casters alone.
bool TraceSceneShadowRay(float3 origin, float3 direction, float tMin, float tMax, uint rayId, out RayHit hit)
{
    return TraceSceneRayMasked(origin, direction, tMin, tMax, true, rayId, RAY_MASK_PROBE_SHADOW, hit);
}

// The hit triangle's normal in world space, unit length, on its front (glTF's counter-clockwise)
// side: a back-face hit's normal points along the ray.
float3 RayHitNormal(RayHit hit)
{
    RayInstance instance = rayInstances[hit.instance];
    BvhTriangle triangle = meshTriangles[hit.triangle];
    float3 local = cross(triangle.e1.xyz, triangle.e2.xyz);
    // Normals carry by the inverse transpose: n_world = transpose(worldToObject) n_local.
    float3 world = local.x * instance.worldToObject[0].xyz + local.y * instance.worldToObject[1].xyz + local.z * instance.worldToObject[2].xyz;
    return normalize(world);
}

// Where a ray leaving a hit point along n starts so it cannot hit the same surface again: the point
// nudged along n by a few ulps of its own coordinates, or by a fixed distance near the origin, where
// ulps are tiny (Wachter and Binder, Ray Tracing Gems chapter 6).
float3 OffsetRayOrigin(float3 position, float3 normal)
{
    const float originScale = 1.0 / 32.0;
    const float floatScale = 1.0 / 65536.0;
    const float intScale = 256.0;
    int3 offset = int3(intScale * normal);
    float3 nudged = asfloat(asint(position) + select(position < float3(0.0), -offset, offset));
    return select(abs(position) < float3(originScale), position + floatScale * normal, nudged);
}

RayMaterial RayHitMaterial(RayHit hit)
{
    return rayMaterials[rayInstances[hit.instance].data.z];
}

#endif
