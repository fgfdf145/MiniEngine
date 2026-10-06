// The ray scene (VulkanRayScene, engine/renderer/vulkan/ray_scene.h) and its traversal: the same
// arrays and the same walk as TraceRay in engine/renderer/ray_tracing_bvh.cpp. A shader that includes
// this binds the ray set at RAY_SCENE_SET (1 unless defined before the include).
//
// Compiled with RAY_QUERY defined (the *_ray_query.comp.spv variants, Vulkan 1.2 SPIR-V), TraceSceneRay
// asks the hardware instead: a ray query against the top-level acceleration structure at binding 5,
// whose instances are the same RayInstances in the same order (VulkanRayAcceleration). The hit it
// reports names the same instance and triangle, so everything after it reads the same arrays. The
// including shader enables GL_EXT_ray_query (an #extension must precede every declaration).

#ifndef RAY_TRACING_COMMON_GLSL
#define RAY_TRACING_COMMON_GLSL

#ifndef RAY_SCENE_SET
#define RAY_SCENE_SET 1
#endif

// kMaxBvhDepth + 1 in ray_tracing_bvh.h covers one level; the two levels share this stack, and the
// top level of a few thousand instances is some 15 deep. A full stack drops the farthest nodes
// rather than overflow.
#define RAY_STACK_SIZE 48

// Instance flags (RayInstance::data.w), matching kRayInstance* in ray_tracing_bvh.h.
#define RAY_INSTANCE_SKIP 1u

// Ray material flags (RayMaterial.emissionFlags.w as uint bits), matching ray_scene.cpp.
#define RAY_MATERIAL_DOUBLE_SIDED 1u

struct BvhNode
{
    vec4 boundsMinFirst; // xyz min, w first as uint bits
    vec4 boundsMaxCount; // xyz max, w count as uint bits (0: inner node)
};

struct BvhTriangle
{
    vec4 v0;
    vec4 e1;
    vec4 e2;
};

struct RayInstance
{
    vec4 worldToObject[3];
    uvec4 data; // x node offset, y triangle offset, z ray material, w flags
};

// The low-frequency material a probe ray sees (ray_material_average.comp).
struct RayMaterial
{
    vec4 albedoCoverage; // rgb average albedo, a the share of the surface that stops rays
    vec4 emissionFlags;  // rgb average emission (cd/m^2), w flags as uint bits
};

layout(std430, set = RAY_SCENE_SET, binding = 0) readonly buffer RayMeshNodes
{
    BvhNode meshNodes[];
};
layout(std430, set = RAY_SCENE_SET, binding = 1) readonly buffer RayMeshTriangles
{
    BvhTriangle meshTriangles[];
};
layout(std430, set = RAY_SCENE_SET, binding = 2) readonly buffer RayInstances
{
    RayInstance rayInstances[];
};
layout(std430, set = RAY_SCENE_SET, binding = 3) readonly buffer RayTopNodes
{
    BvhNode topNodes[];
};
layout(std430, set = RAY_SCENE_SET, binding = 4) readonly buffer RayMaterials
{
    RayMaterial rayMaterials[];
};

#ifdef RAY_QUERY
layout(set = RAY_SCENE_SET, binding = 5) uniform accelerationStructureEXT rayTopLevel;
#endif

struct RayHit
{
    float t;
    uint instance;
    uint triangle;
    vec2 barycentrics;
    bool frontFace;
};

// 1 / direction, with components too small to invert replaced by +-1e-20 (sign kept, zero positive).
vec3 SafeInverse(vec3 direction)
{
    const float tiny = 1e-20;
    vec3 floorValue = vec3(tiny) * (step(0.0, direction) * 2.0 - 1.0);
    return 1.0 / mix(floorValue, direction, greaterThan(abs(direction), vec3(tiny)));
}

// The entry distance when the ray meets the box within [tMin, tMax], else a negative number.
float IntersectBounds(vec3 boundsMin, vec3 boundsMax, vec3 origin, vec3 inverseDirection, float tMin, float tMax)
{
    vec3 t0 = (boundsMin - origin) * inverseDirection;
    vec3 t1 = (boundsMax - origin) * inverseDirection;
    vec3 near = min(t0, t1);
    vec3 far = max(t0, t1);
    float enter = max(max(near.x, near.y), max(near.z, tMin));
    float exit = min(min(far.x, far.y), min(far.z, tMax));
    return enter <= exit ? enter : -1.0;
}

// Moller-Trumbore; frontFace when the triangle's counter-clockwise side faces the ray.
bool IntersectTriangle(BvhTriangle triangle, vec3 origin, vec3 direction, float tMin, float tMax, out float t, out vec2 barycentrics, out bool frontFace)
{
    t = 0.0;
    barycentrics = vec2(0.0);
    frontFace = false;
    vec3 e1 = triangle.e1.xyz;
    vec3 e2 = triangle.e2.xyz;
    vec3 p = cross(direction, e2);
    float determinant = dot(e1, p);
    if (determinant == 0.0)
    {
        return false;
    }
    float inverseDeterminant = 1.0 / determinant;
    vec3 s = origin - triangle.v0.xyz;
    float u = dot(s, p) * inverseDeterminant;
    if (u < 0.0 || u > 1.0)
    {
        return false;
    }
    vec3 q = cross(s, e1);
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
    barycentrics = vec2(u, v);
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

// Whether a candidate hit stops the ray: a partly covered surface (foliage cards, glass) stops the
// share of rays its coverage says, decided per ray and triangle so a ray agrees with itself.
bool AcceptHit(uint instance, uint triangle, uint rayId)
{
    float coverage = rayMaterials[rayInstances[instance].data.z].albedoCoverage.a;
    return coverage >= 1.0 || RayHash(rayId, triangle) < coverage;
}

#ifdef RAY_QUERY
// The nearest accepted hit within (tMin, tMax), or the first found when anyHit (shadow rays). rayId
// seeds the coverage decisions. Instances whose material stops every ray are opaque to the hardware
// (VulkanRayAcceleration::UpdateTopLevel); the others' candidates come back here for AcceptHit.
bool TraceSceneRay(vec3 origin, vec3 direction, float tMin, float tMax, bool anyHit, uint rayId, out RayHit hit)
{
    hit.t = tMax;
    hit.instance = 0u;
    hit.triangle = 0u;
    hit.barycentrics = vec2(0.0);
    hit.frontFace = false;

    rayQueryEXT query;
    uint flags = anyHit ? gl_RayFlagsTerminateOnFirstHitEXT : gl_RayFlagsNoneEXT;
    rayQueryInitializeEXT(query, rayTopLevel, flags, 0xFFu, origin, tMin, direction, tMax);
    while (rayQueryProceedEXT(query))
    {
        if (rayQueryGetIntersectionTypeEXT(query, false) != gl_RayQueryCandidateIntersectionTriangleEXT)
        {
            continue;
        }
        uint instance = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(query, false));
        uint triangle = rayInstances[instance].data.y + uint(rayQueryGetIntersectionPrimitiveIndexEXT(query, false));
        if (AcceptHit(instance, triangle, rayId))
        {
            rayQueryConfirmIntersectionEXT(query);
        }
    }
    if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT)
    {
        return false;
    }
    hit.t = rayQueryGetIntersectionTEXT(query, true);
    hit.instance = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(query, true));
    hit.triangle = rayInstances[hit.instance].data.y + uint(rayQueryGetIntersectionPrimitiveIndexEXT(query, true));
    hit.barycentrics = rayQueryGetIntersectionBarycentricsEXT(query, true);
    // The compute walk's rule (IntersectTriangle's determinant) from the triangle itself, rather than
    // the API's facing convention: the counter-clockwise side faces a ray running against its normal.
    BvhTriangle triangle = meshTriangles[hit.triangle];
    vec3 objectDirection = rayQueryGetIntersectionObjectRayDirectionEXT(query, true);
    hit.frontFace = dot(objectDirection, cross(triangle.e1.xyz, triangle.e2.xyz)) < 0.0;
    return true;
}
#else
// The nearest accepted hit within (tMin, tMax), or the first found when anyHit (shadow rays). rayId
// seeds the coverage decisions. Both hierarchy levels share one stack: an instance's nodes are pushed
// over the top level's and all popped before the next top-level entry, so the current instance's
// transformed ray is the right one for every mesh entry popped.
bool TraceSceneRay(vec3 origin, vec3 direction, float tMin, float tMax, bool anyHit, uint rayId, out RayHit hit)
{
    hit.t = tMax;
    hit.instance = 0u;
    hit.triangle = 0u;
    hit.barycentrics = vec2(0.0);
    hit.frontFace = false;
    bool found = false;

    const uint MESH_ENTRY = 0x80000000u;
    uint stack[RAY_STACK_SIZE];
    uint size = 0u;
    stack[size++] = 0u;

    vec3 worldInverse = SafeInverse(direction);
    uint instanceIndex = 0u;
    vec3 localOrigin = origin;
    vec3 localDirection = direction;
    vec3 localInverse = worldInverse;
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
            uint first = floatBitsToUint(node.boundsMinFirst.w);
            uint count = floatBitsToUint(node.boundsMaxCount.w);
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
            if ((instance.data.w & RAY_INSTANCE_SKIP) != 0u)
            {
                continue;
            }
            instanceIndex = first;
            localOrigin = vec3(dot(instance.worldToObject[0], vec4(origin, 1.0)), dot(instance.worldToObject[1], vec4(origin, 1.0)), dot(instance.worldToObject[2], vec4(origin, 1.0)));
            localDirection = vec3(dot(instance.worldToObject[0].xyz, direction), dot(instance.worldToObject[1].xyz, direction), dot(instance.worldToObject[2].xyz, direction));
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
        uint first = floatBitsToUint(node.boundsMinFirst.w);
        uint count = floatBitsToUint(node.boundsMaxCount.w);
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
            vec2 barycentrics;
            bool frontFace;
            if (!IntersectTriangle(meshTriangles[triangleOffset + k], localOrigin, localDirection, tMin, hit.t, t, barycentrics, frontFace))
            {
                continue;
            }
            if (!AcceptHit(instanceIndex, triangleOffset + k, rayId))
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

// The hit triangle's normal in world space, unit length, on its front (glTF's counter-clockwise)
// side: a back-face hit's normal points along the ray.
vec3 RayHitNormal(RayHit hit)
{
    RayInstance instance = rayInstances[hit.instance];
    BvhTriangle triangle = meshTriangles[hit.triangle];
    vec3 local = cross(triangle.e1.xyz, triangle.e2.xyz);
    // Normals carry by the inverse transpose: n_world = transpose(worldToObject) n_local.
    vec3 world = local.x * instance.worldToObject[0].xyz + local.y * instance.worldToObject[1].xyz + local.z * instance.worldToObject[2].xyz;
    return normalize(world);
}

// Where a ray leaving a hit point along n starts so it cannot hit the same surface again: the point
// nudged along n by a few ulps of its own coordinates, or by a fixed distance near the origin, where
// ulps are tiny (Wachter and Binder, Ray Tracing Gems chapter 6).
vec3 OffsetRayOrigin(vec3 position, vec3 normal)
{
    const float originScale = 1.0 / 32.0;
    const float floatScale = 1.0 / 65536.0;
    const float intScale = 256.0;
    ivec3 offset = ivec3(intScale * normal);
    vec3 nudged = intBitsToFloat(floatBitsToInt(position) + mix(offset, -offset, lessThan(position, vec3(0.0))));
    return mix(nudged, position + floatScale * normal, lessThan(abs(position), vec3(originScale)));
}

RayMaterial RayHitMaterial(RayHit hit)
{
    return rayMaterials[rayInstances[hit.instance].data.z];
}

#endif
