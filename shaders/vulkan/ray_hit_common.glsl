// Hit shading for hardware ray tracing (docs/design/2026-10-07-ray-traced-effects-design.md): what a
// ray that hit something finds there, read from the hit mesh's own vertex and index buffers (their
// device addresses, ray set binding 6) and its draw slot's material (set 0 bindings 12 and 17) and
// textures (the ray texture table, VulkanRayScene::GetTextureSet). Only the ray query variants of a
// shader have it. Include after scene_common.glsl, ray_tracing_common.glsl, material_common.glsl and
// material_uv.glsl; the including shader enables GL_EXT_buffer_reference,
// GL_EXT_buffer_reference_uvec2 and GL_EXT_nonuniform_qualifier.

#ifndef RAY_HIT_COMMON_GLSL
#define RAY_HIT_COMMON_GLSL

#ifndef RAY_TEXTURE_SET
#define RAY_TEXTURE_SET 3
#endif

// Vertex in engine/asset/mesh.h, 17 floats: position, colour, UV 0, normal, tangent, UV 1
// (ray_scene.cpp checks the layout).
#define RAY_VERTEX_FLOATS 17u
#define RAY_VERTEX_COLOR 3u
#define RAY_VERTEX_UV0 6u
#define RAY_VERTEX_NORMAL 8u
#define RAY_VERTEX_UV1 15u

// kRayTexturesPerSlot and their order (VulkanRayScene::WriteTextureSlot).
#define RAY_TEXTURES_PER_SLOT 4u
#define RAY_TEXTURE_BASE_COLOR 0u
#define RAY_TEXTURE_METALLIC 1u
#define RAY_TEXTURE_ROUGHNESS 2u
#define RAY_TEXTURE_EMISSIVE 3u

// kRayMaterialAlphaMask in ray_scene.cpp.
#define RAY_MATERIAL_ALPHA_MASK 2u

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RayVertexFloats
{
    float values[];
};
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer RayIndexList
{
    uint values[];
};

// RayMeshGeometry in ray_scene.cpp: the mesh's vertex and index buffer addresses.
struct RayMeshGeometry
{
    uvec2 vertices;
    uvec2 indices;
};
layout(std430, set = RAY_SCENE_SET, binding = 6) readonly buffer RayMeshGeometries
{
    RayMeshGeometry rayMeshGeometries[];
};
// Each leaf triangle's index in its mesh's index list / 3, laid out as meshTriangles.
layout(std430, set = RAY_SCENE_SET, binding = 7) readonly buffer RaySourceTriangles
{
    uint raySourceTriangles[];
};
layout(set = RAY_TEXTURE_SET, binding = 0) uniform sampler2D rayTextures[];

// The three corners of a hit triangle, as indices into its mesh's vertex buffer.
struct RayHitCorners
{
    RayVertexFloats vertices;
    uvec3 index;
};

RayHitCorners RayHitCornersOf(uint instance, uint triangle)
{
    RayMeshGeometry geometry = rayMeshGeometries[rayInstances[instance].data.w >> RAY_INSTANCE_MESH_SHIFT];
    RayIndexList indices = RayIndexList(geometry.indices);
    uint source = raySourceTriangles[triangle] * 3u;
    RayHitCorners corners;
    corners.vertices = RayVertexFloats(geometry.vertices);
    corners.index = uvec3(indices.values[source], indices.values[source + 1u], indices.values[source + 2u]);
    return corners;
}

vec2 RayCornerVec2(RayHitCorners corners, uint corner, uint offset)
{
    uint base = corners.index[corner] * RAY_VERTEX_FLOATS + offset;
    return vec2(corners.vertices.values[base], corners.vertices.values[base + 1u]);
}

vec3 RayCornerVec3(RayHitCorners corners, uint corner, uint offset)
{
    uint base = corners.index[corner] * RAY_VERTEX_FLOATS + offset;
    return vec3(corners.vertices.values[base], corners.vertices.values[base + 1u], corners.vertices.values[base + 2u]);
}

// A vertex attribute at the hit: barycentrics are v1's and v2's weights, as RayHit has them.
vec2 RayInterpolate2(RayHitCorners corners, uint offset, vec2 barycentrics)
{
    return RayCornerVec2(corners, 0u, offset) * (1.0 - barycentrics.x - barycentrics.y) +
           RayCornerVec2(corners, 1u, offset) * barycentrics.x +
           RayCornerVec2(corners, 2u, offset) * barycentrics.y;
}

vec3 RayInterpolate3(RayHitCorners corners, uint offset, vec2 barycentrics)
{
    return RayCornerVec3(corners, 0u, offset) * (1.0 - barycentrics.x - barycentrics.y) +
           RayCornerVec3(corners, 1u, offset) * barycentrics.x +
           RayCornerVec3(corners, 2u, offset) * barycentrics.y;
}

// The surface a ray found.
struct RayHitSurface
{
    vec3 position;
    // World space, unit length, both on the side the ray arrived from: the triangle's own normal and
    // the interpolated vertex normal (no normal map).
    vec3 faceNormal;
    vec3 normal;
    vec2 uv0;
    vec2 uv1;
    vec3 color;
    uint drawSlot;
    // log2 of the texture-space area per world-space area of the triangle, per texel of a 1 x 1
    // texture: half of it plus log2(texture size) is the mip a footprint of one world unit wants.
    float log2TexelDensity;
};

vec3 RayObjectToWorldNormal(RayInstance instance, vec3 normal)
{
    // The inverse transpose of the model matrix: transpose(worldToObject).
    return normal.x * instance.worldToObject[0].xyz + normal.y * instance.worldToObject[1].xyz + normal.z * instance.worldToObject[2].xyz;
}

RayHitSurface RayHitSurfaceOf(RayHit hit, vec3 origin, vec3 direction)
{
    RayInstance instance = rayInstances[hit.instance];
    RayHitCorners corners = RayHitCornersOf(hit.instance, hit.triangle);
    RayHitSurface surface;
    surface.position = origin + direction * hit.t;
    surface.drawSlot = instance.data.z;
    surface.uv0 = RayInterpolate2(corners, RAY_VERTEX_UV0, hit.barycentrics);
    surface.uv1 = RayInterpolate2(corners, RAY_VERTEX_UV1, hit.barycentrics);
    surface.color = RayInterpolate3(corners, RAY_VERTEX_COLOR, hit.barycentrics);

    vec3 towardRay = -direction;
    surface.faceNormal = RayHitNormal(hit);
    if (dot(surface.faceNormal, towardRay) < 0.0)
    {
        surface.faceNormal = -surface.faceNormal;
    }
    vec3 normal = RayObjectToWorldNormal(instance, RayInterpolate3(corners, RAY_VERTEX_NORMAL, hit.barycentrics));
    float length2 = dot(normal, normal);
    normal = length2 > 1e-12 ? normal * inversesqrt(length2) : surface.faceNormal;
    surface.normal = dot(normal, surface.faceNormal) < 0.0 ? -normal : normal;

    // The texel density from the triangle's UV and object-space areas, the latter scaled to world
    // space by the instance's mean scale (worldToObject's rows are 1 / scale long).
    BvhTriangle triangle = meshTriangles[hit.triangle];
    float objectArea = length(cross(triangle.e1.xyz, triangle.e2.xyz));
    float inverseScale = (length(instance.worldToObject[0].xyz) + length(instance.worldToObject[1].xyz) + length(instance.worldToObject[2].xyz)) / 3.0;
    float worldArea = objectArea / max(inverseScale * inverseScale, 1e-12);
    vec2 uvA = RayCornerVec2(corners, 0u, RAY_VERTEX_UV0);
    vec2 uvB = RayCornerVec2(corners, 1u, RAY_VERTEX_UV0) - uvA;
    vec2 uvC = RayCornerVec2(corners, 2u, RAY_VERTEX_UV0) - uvA;
    float uvArea = abs(uvB.x * uvC.y - uvB.y * uvC.x);
    surface.log2TexelDensity = log2(max(uvArea, 1e-12) / max(worldArea, 1e-12));
    return surface;
}

// The mip for a ray cone of footprintWidth (world units across, at the hit) meeting the surface at
// cosine cosTheta (Akenine-Moller et al. 2019, "Texture Level of Detail Strategies for Real-Time Ray
// Tracing").
float RayTextureLod(RayHitSurface surface, uint textureIndex, float footprintWidth, float cosTheta)
{
    vec2 size = vec2(textureSize(rayTextures[nonuniformEXT(textureIndex)], 0));
    return 0.5 * (surface.log2TexelDensity + log2(size.x * size.y)) + log2(max(footprintWidth, 1e-6) / max(cosTheta, 0.05));
}

vec4 RayTexture(RayHitSurface surface, MaterialData material, uint slotTexture, uint materialTextureSlot, float footprintWidth, float cosTheta)
{
    uint textureIndex = surface.drawSlot * RAY_TEXTURES_PER_SLOT + slotTexture;
    vec2 uv = MaterialSlotUv(material, surface.drawSlot, materialTextureSlot, surface.uv0, surface.uv1);
    return textureLod(rayTextures[nonuniformEXT(textureIndex)], uv, RayTextureLod(surface, textureIndex, footprintWidth, cosTheta));
}

// The plain base of the material at the hit: base colour (times the vertex colour), metallic,
// roughness and emission, through each texture's transform. The blend graph's second layer, detail
// layers, normal maps and the layered extensions are left out; a reflection or a bounce off the
// surface hardly shows them.
struct RayHitShading
{
    vec3 albedo;
    float alpha;
    float metallic;
    float roughness;
    // Physical units (cd/m^2).
    vec3 emissive;
    uint flags;
};

RayHitShading RayHitShadingOf(RayHitSurface surface, float footprintWidth, float cosTheta)
{
    MaterialData material = materialData.materials[surface.drawSlot];
    vec4 baseColor = RayTexture(surface, material, RAY_TEXTURE_BASE_COLOR, 0u, footprintWidth, cosTheta) * vec4(surface.color, 1.0) * material.baseColorFactor;
    RayHitShading result;
    result.albedo = clamp(baseColor.rgb, 0.0, 1.0);
    result.alpha = baseColor.a;
    result.metallic = clamp(material.surfaceFactors.x * RayTexture(surface, material, RAY_TEXTURE_METALLIC, 2u, footprintWidth, cosTheta).b, 0.0, 1.0);
    result.roughness = clamp(material.surfaceFactors.y * RayTexture(surface, material, RAY_TEXTURE_ROUGHNESS, 3u, footprintWidth, cosTheta).g, 0.04, 1.0);
    result.emissive = RayTexture(surface, material, RAY_TEXTURE_EMISSIVE, 5u, footprintWidth, cosTheta).rgb * material.emissiveFactor;
    result.flags = material.shadingModel.x;
    return result;
}

// The textured coverage test the visibility rays run on a candidate hit (RAY_TEXTURED_ALPHA in
// ray_tracing_common.glsl): an alpha-tested surface (foliage, fences) stops a ray where its base
// colour's alpha reaches the cutoff, so its shadow has the leaves' shapes rather than the averaged
// coverage's dither. Read at a mip about 256 texels across, which keeps the shapes and the cache.
// Other partly covered surfaces (transmission) keep the coverage decision.
bool AcceptTexturedHit(uint instance, uint triangle, vec2 barycentrics, uint rayId)
{
    RayMaterial rayMaterial = rayMaterials[rayInstances[instance].data.z];
    float coverage = rayMaterial.albedoCoverage.a;
    if (coverage >= 1.0)
    {
        return true;
    }
    if ((floatBitsToUint(rayMaterial.emissionFlags.w) & RAY_MATERIAL_ALPHA_MASK) == 0u)
    {
        return RayHash(rayId, triangle) < coverage;
    }
    uint drawSlot = rayInstances[instance].data.z;
    MaterialData material = materialData.materials[drawSlot];
    RayHitCorners corners = RayHitCornersOf(instance, triangle);
    vec2 uv0 = RayInterpolate2(corners, RAY_VERTEX_UV0, barycentrics);
    vec2 uv1 = (material.shadingModel.y != 0u) ? RayInterpolate2(corners, RAY_VERTEX_UV1, barycentrics) : uv0;
    uint textureIndex = drawSlot * RAY_TEXTURES_PER_SLOT + RAY_TEXTURE_BASE_COLOR;
    float width = float(textureSize(rayTextures[nonuniformEXT(textureIndex)], 0).x);
    float lod = max(log2(width / 256.0), 0.0);
    vec2 uv = MaterialSlotUv(material, drawSlot, 0u, uv0, uv1);
    float alpha = textureLod(rayTextures[nonuniformEXT(textureIndex)], uv, lod).a * material.baseColorFactor.a;
    return alpha >= material.alphaCutoff;
}

#endif
