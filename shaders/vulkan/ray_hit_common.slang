// Hit shading for hardware ray tracing (docs/design/2026-10-07-ray-traced-effects-design.md): what a
// ray that hit something finds there, read from the hit mesh's own vertex and index buffers (their
// device addresses, ray set binding 6) and its draw slot's material (set 0 bindings 12 and 17) and
// textures (the ray texture table, VulkanRayScene::GetTextureSet). Only the ray query variants of a
// shader have it. Include after scene_common.slang, ray_tracing_common.slang, material_common.slang and
// material_uv.slang. The buffers are read through pointers, which need the device's shaderInt64.

#ifndef RAY_HIT_COMMON_SLANG
#define RAY_HIT_COMMON_SLANG

#include "shader_helpers.slang"
#include "normal_map.slang"

#ifndef RAY_TEXTURE_SET
#define RAY_TEXTURE_SET 3
#endif

// Vertex in engine/asset/mesh.h, 20 floats: position, colour, UV 0, normal, tangent, UV 1, outline normal
// (ray_scene.cpp checks the layout).
#define RAY_VERTEX_FLOATS 20u
#define RAY_VERTEX_COLOR 3u
#define RAY_VERTEX_UV0 6u
#define RAY_VERTEX_NORMAL 8u
#define RAY_VERTEX_TANGENT 11u
#define RAY_VERTEX_UV1 15u

// kRayTexturesPerSlot and their order (VulkanRayScene::WriteTextureSlot).
#define RAY_TEXTURES_PER_SLOT 5u
#define RAY_TEXTURE_BASE_COLOR 0u
#define RAY_TEXTURE_METALLIC 1u
#define RAY_TEXTURE_ROUGHNESS 2u
#define RAY_TEXTURE_EMISSIVE 3u
#define RAY_TEXTURE_NORMAL 4u

// RayMeshGeometry in ray_scene.cpp: the mesh's vertex and index buffer addresses.
struct RayMeshGeometry
{
    uint2 vertices;
    uint2 indices;
};
// A device address from the two 32-bit halves RayMeshGeometry stores.
uint64_t RayDeviceAddress(uint2 address)
{
    return uint64_t(address.x) | (uint64_t(address.y) << 32);
}
[[vk::binding(6, RAY_SCENE_SET)]] StructuredBuffer<RayMeshGeometry> rayMeshGeometries;
// Each leaf triangle's index in its mesh's index list / 3, laid out as meshTriangles.
[[vk::binding(7, RAY_SCENE_SET)]] StructuredBuffer<uint> raySourceTriangles;
[[vk::binding(0, RAY_TEXTURE_SET)]] Sampler2D rayTextures[];

// The three corners of a hit triangle, as indices into its mesh's vertex buffer.
struct RayHitCorners
{
    float* vertices;
    uint3 index;
};

RayHitCorners RayHitCornersOf(uint instance, uint triangle)
{
    RayMeshGeometry geometry = rayMeshGeometries[rayInstances[instance].data.w >> RAY_INSTANCE_MESH_SHIFT];
    uint* indices = (uint*)RayDeviceAddress(geometry.indices);
    uint source = raySourceTriangles[triangle] * 3u;
    RayHitCorners corners;
    corners.vertices = (float*)RayDeviceAddress(geometry.vertices);
    corners.index = uint3(indices[source], indices[source + 1u], indices[source + 2u]);
    return corners;
}

float2 RayCornerVec2(RayHitCorners corners, uint corner, uint offset)
{
    uint base = corners.index[corner] * RAY_VERTEX_FLOATS + offset;
    return float2(corners.vertices[base], corners.vertices[base + 1u]);
}

float3 RayCornerVec3(RayHitCorners corners, uint corner, uint offset)
{
    uint base = corners.index[corner] * RAY_VERTEX_FLOATS + offset;
    return float3(corners.vertices[base], corners.vertices[base + 1u], corners.vertices[base + 2u]);
}

// A vertex attribute at the hit: barycentrics are v1's and v2's weights, as RayHit has them.
float2 RayInterpolate2(RayHitCorners corners, uint offset, float2 barycentrics)
{
    return RayCornerVec2(corners, 0u, offset) * (1.0 - barycentrics.x - barycentrics.y) +
           RayCornerVec2(corners, 1u, offset) * barycentrics.x +
           RayCornerVec2(corners, 2u, offset) * barycentrics.y;
}

float3 RayInterpolate3(RayHitCorners corners, uint offset, float2 barycentrics)
{
    return RayCornerVec3(corners, 0u, offset) * (1.0 - barycentrics.x - barycentrics.y) +
           RayCornerVec3(corners, 1u, offset) * barycentrics.x +
           RayCornerVec3(corners, 2u, offset) * barycentrics.y;
}

// The surface a ray found.
struct RayHitSurface
{
    float3 position;
    // World space, unit length, both on the side the ray arrived from: the triangle's own normal and
    // the interpolated vertex normal (no normal map).
    float3 faceNormal;
    float3 normal;
    float2 uv0;
    float2 uv1;
    float3 color;
    uint drawSlot;
    // log2 of the texture-space area per world-space area of the triangle, per texel of a 1 x 1
    // texture: half of it plus log2(texture size) is the mip a footprint of one world unit wants.
    float log2TexelDensity;
};

float3 RayObjectToWorldNormal(RayInstance instance, float3 normal)
{
    // The inverse transpose of the model matrix: transpose(worldToObject).
    return normal.x * instance.worldToObject[0].xyz + normal.y * instance.worldToObject[1].xyz + normal.z * instance.worldToObject[2].xyz;
}

RayHitSurface RayHitSurfaceOf(RayHit hit, float3 origin, float3 direction)
{
    RayInstance instance = rayInstances[hit.instance];
    RayHitCorners corners = RayHitCornersOf(hit.instance, hit.triangle);
    RayHitSurface surface;
    surface.position = origin + direction * hit.t;
    surface.drawSlot = instance.data.z;
    surface.uv0 = RayInterpolate2(corners, RAY_VERTEX_UV0, hit.barycentrics);
    surface.uv1 = RayInterpolate2(corners, RAY_VERTEX_UV1, hit.barycentrics);
    surface.color = RayInterpolate3(corners, RAY_VERTEX_COLOR, hit.barycentrics);

    float3 towardRay = -direction;
    surface.faceNormal = RayHitNormal(hit);
    if (dot(surface.faceNormal, towardRay) < 0.0)
    {
        surface.faceNormal = -surface.faceNormal;
    }
    float3 normal = RayObjectToWorldNormal(instance, RayInterpolate3(corners, RAY_VERTEX_NORMAL, hit.barycentrics));
    float length2 = dot(normal, normal);
    normal = length2 > 1e-12 ? normal * rsqrt(length2) : surface.faceNormal;
    surface.normal = dot(normal, surface.faceNormal) < 0.0 ? -normal : normal;

    // The texel density from the triangle's UV and object-space areas, the latter scaled to world
    // space by the instance's mean scale (worldToObject's rows are 1 / scale long).
    BvhTriangle triangle = meshTriangles[hit.triangle];
    float objectArea = length(cross(triangle.e1.xyz, triangle.e2.xyz));
    float inverseScale = (length(instance.worldToObject[0].xyz) + length(instance.worldToObject[1].xyz) + length(instance.worldToObject[2].xyz)) / 3.0;
    float worldArea = objectArea / max(inverseScale * inverseScale, 1e-12);
    float2 uvA = RayCornerVec2(corners, 0u, RAY_VERTEX_UV0);
    float2 uvB = RayCornerVec2(corners, 1u, RAY_VERTEX_UV0) - uvA;
    float2 uvC = RayCornerVec2(corners, 2u, RAY_VERTEX_UV0) - uvA;
    float uvArea = abs(uvB.x * uvC.y - uvB.y * uvC.x);
    surface.log2TexelDensity = log2(max(uvArea, 1e-12) / max(worldArea, 1e-12));
    return surface;
}

// The mip for a ray cone of footprintWidth (world units across, at the hit) meeting the surface at
// cosine cosTheta (Akenine-Moller et al. 2019, "Texture Level of Detail Strategies for Real-Time Ray
// Tracing").
float RayTextureLod(RayHitSurface surface, uint textureIndex, float footprintWidth, float cosTheta)
{
    float2 size = float2(TextureSize(rayTextures[NonUniformResourceIndex(textureIndex)], 0));
    return 0.5 * (surface.log2TexelDensity + log2(size.x * size.y)) + log2(max(footprintWidth, 1e-6) / max(cosTheta, 0.05));
}

float4 RayTexture(RayHitSurface surface, MaterialData material, uint slotTexture, uint materialTextureSlot, float footprintWidth, float cosTheta)
{
    uint textureIndex = surface.drawSlot * RAY_TEXTURES_PER_SLOT + slotTexture;
    float2 uv = MaterialSlotUv(material, surface.drawSlot, materialTextureSlot, surface.uv0, surface.uv1);
    return rayTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(uv, RayTextureLod(surface, textureIndex, footprintWidth, cosTheta));
}

// The plain base of the material at the hit: base colour (times the vertex colour), metallic,
// roughness and emission, through each texture's transform. The blend graph's second layer, detail
// layers, normal maps and the layered extensions are left out; a reflection or a bounce off the
// surface hardly shows them.
struct RayHitShading
{
    float3 albedo;
    float alpha;
    float metallic;
    float roughness;
    // Physical units (cd/m^2).
    float3 emissive;
    uint flags;
};

RayHitShading RayHitShadingOf(RayHitSurface surface, float footprintWidth, float cosTheta)
{
    MaterialData material = materials[surface.drawSlot];
    float4 baseColor = RayTexture(surface, material, RAY_TEXTURE_BASE_COLOR, 0u, footprintWidth, cosTheta) * float4(surface.color, 1.0) * material.baseColorFactor;
    RayHitShading result;
    result.albedo = clamp(baseColor.rgb, 0.0, 1.0);
    result.alpha = baseColor.a;
    result.metallic = clamp(material.surfaceFactors.x * RayTexture(surface, material, RAY_TEXTURE_METALLIC, 2u, footprintWidth, cosTheta).b, 0.0, 1.0);
    result.roughness = clamp(material.surfaceFactors.y * RayTexture(surface, material, RAY_TEXTURE_ROUGHNESS, 3u, footprintWidth, cosTheta).g, 0.04, 1.0);
    result.emissive = RayTexture(surface, material, RAY_TEXTURE_EMISSIVE, 5u, footprintWidth, cosTheta).rgb * material.emissiveFactor;
    result.flags = material.shadingModel.x;
    return result;
}

// The hit's shading normal with its material's normal map, where it has one (the main map through its
// transform, times the normal scale) in the frame of the interpolated vertex normal and tangent, the frame mirrored with
// the normal for a hit on its back, as gbuffer.frag mirrors it: what the path tracer shades its later
// surfaces with. The map's level of detail is RayTexture's for footprintWidth and cosTheta. The tangent
// goes to world space as the normal does, then into the normal's plane, which keeps its direction
// under every transform but a non-uniform scale.
float3 RayHitMappedNormal(RayHit hit, RayHitSurface surface, float footprintWidth, float cosTheta)
{
    MaterialData material = materials[surface.drawSlot];
    if (material.surfaceFactors.z == 0.0 || (asuint(rayMaterials[surface.drawSlot].emissionFlags.w) & RAY_MATERIAL_NORMAL_MAP) == 0u)
    {
        return surface.normal;
    }
    RayInstance instance = rayInstances[hit.instance];
    RayHitCorners corners = RayHitCornersOf(hit.instance, hit.triangle);
    float3 vertexNormal = RayObjectToWorldNormal(instance, RayInterpolate3(corners, RAY_VERTEX_NORMAL, hit.barycentrics));
    float faceSign = dot(vertexNormal, surface.normal) < 0.0 ? -1.0 : 1.0;
    float3 tangent = RayObjectToWorldNormal(instance, RayInterpolate3(corners, RAY_VERTEX_TANGENT, hit.barycentrics)) * faceSign;
    tangent -= surface.normal * dot(surface.normal, tangent);
    float tangentLength2 = dot(tangent, tangent);
    if (tangentLength2 < 1e-12)
    {
        return surface.normal;
    }
    tangent *= rsqrt(tangentLength2);
    float handedness = corners.vertices[corners.index[0] * RAY_VERTEX_FLOATS + RAY_VERTEX_TANGENT + 3u] < 0.0 ? -1.0 : 1.0;
    float3 bitangent = cross(surface.normal, tangent) * (handedness * faceSign);
    float3 mapped = DecodeNormalMap(RayTexture(surface, material, RAY_TEXTURE_NORMAL, 1u, footprintWidth, cosTheta));
    mapped.xy = RotateMaterialTangentXy(material, surface.drawSlot, 1u, mapped.xy) * material.surfaceFactors.z;
    float3 N = mul(MatrixFromColumns(tangent, bitangent, surface.normal), mapped);
    float length2 = dot(N, N);
    return length2 > 1e-12 ? N * rsqrt(length2) : surface.normal;
}

// The textured coverage test the visibility rays run on a candidate hit (RAY_TEXTURED_ALPHA in
// ray_tracing_common.slang): an alpha-tested surface (foliage, fences) stops a ray where its base
// colour's alpha reaches the cutoff, so its shadow has the leaves' shapes rather than the averaged
// coverage's dither; an alpha-blended one (glass, which only the path tracer's rays meet) stops the
// share of rays its alpha there says. Read at a mip about 256 texels across, which keeps the shapes
// and the cache. Other partly covered surfaces (transmission) keep the coverage decision.
bool AcceptTexturedHit(uint instance, uint triangle, float2 barycentrics, uint rayId)
{
    RayMaterial rayMaterial = rayMaterials[rayInstances[instance].data.z];
    float coverage = rayMaterial.albedoCoverage.a;
    if (coverage >= 1.0)
    {
        return true;
    }
    uint materialFlags = asuint(rayMaterial.emissionFlags.w);
    if ((materialFlags & (RAY_MATERIAL_ALPHA_MASK | RAY_MATERIAL_ALPHA_BLEND)) == 0u)
    {
        return RayHash(rayId, triangle) < coverage;
    }
    uint drawSlot = rayInstances[instance].data.z;
    MaterialData material = materials[drawSlot];
    RayHitCorners corners = RayHitCornersOf(instance, triangle);
    float2 uv0 = RayInterpolate2(corners, RAY_VERTEX_UV0, barycentrics);
    float2 uv1 = (material.shadingModel.y != 0u) ? RayInterpolate2(corners, RAY_VERTEX_UV1, barycentrics) : uv0;
    uint textureIndex = drawSlot * RAY_TEXTURES_PER_SLOT + RAY_TEXTURE_BASE_COLOR;
    float width = float(TextureSize(rayTextures[NonUniformResourceIndex(textureIndex)], 0).x);
    float lod = max(log2(width / 256.0), 0.0);
    float2 uv = MaterialSlotUv(material, drawSlot, 0u, uv0, uv1);
    float alpha = rayTextures[NonUniformResourceIndex(textureIndex)].SampleLevel(uv, lod).a * material.baseColorFactor.a;
    return (materialFlags & RAY_MATERIAL_ALPHA_BLEND) != 0u ? RayHash(rayId, triangle) < alpha : alpha >= material.alphaCutoff;
}

#endif
