#pragma once

#include "material_preview_environment.h"
#include "material_preview_textures.h"

#include <engine/asset/mesh.h>
#include <engine/renderer/material.h>
#include <engine/renderer/ray_tracing_bvh.h>
#include <engine/renderer/renderer_world.h>

#include <glm/glm.hpp>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace me
{

// The preview's unit of radiance in the engine's (cd/m^2): a white surface in daylight
// (kAcNeutralLuminance in the kn5 import), which is what a white surface comes to under the
// preview's lights. Emission, given in cd/m^2, is divided by it.
inline constexpr float kMaterialPreviewNitsPerUnit = 25500.0f;

// One mesh's material as the preview shades it: the GPU material the scene draws the submesh with
// (FillRenderSubmeshMaterial), and the textures its paths name. A texture still loading, or with no
// path, samples as the renderer's default for its slot (white; flat for a normal map).
struct MaterialPreviewMaterial
{
    GpuMaterialData material;
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    bool doubleSided = false;
    // Per slot, ComputeTextureTransformRows' two rows; read when material.shadingModel[1] is set.
    std::array<std::array<float, 8>, kMaterialTextureSlotCount> transformRows{};
    MaterialTextureSamplers samplers{};
    std::array<std::shared_ptr<const MaterialPreviewTexture>, kMaterialTextureSlotCount> textures{};
    std::shared_ptr<const MaterialPreviewTexture> detailMask;
    std::array<std::shared_ptr<const MaterialPreviewTexture>, kDetailLayerCount> detailLayers{};
};

// Every texture file the submesh's material samples, for the cache to load.
std::vector<std::string> MaterialPreviewTexturePaths(const CpuRenderSubmesh& submesh);
// The submesh's material (filled by FillRenderSubmeshMaterial) with the textures the cache has.
MaterialPreviewMaterial BuildMaterialPreviewMaterial(const CpuRenderSubmesh& submesh, const MaterialPreviewTextureCache& textures);

// A point on a triangle as the vertex stage would hand it to gbuffer.frag, and how much of the
// surface a pixel covers there (a ray cone), for the texture level of detail.
struct MaterialPreviewSurfacePoint
{
    glm::vec3 position{0.0f};
    // The triangle's own normal as it is wound (counter-clockwise front), unit length.
    glm::vec3 faceNormal{0.0f, 1.0f, 0.0f};
    glm::vec3 vertexNormal{0.0f, 1.0f, 0.0f};
    glm::vec4 vertexTangent{1.0f, 0.0f, 0.0f, 1.0f};
    glm::vec2 uv0{0.0f};
    glm::vec2 uv1{0.0f};
    glm::vec3 color{1.0f};
    bool frontFacing = true;
    // World units a pixel covers on the surface there.
    float footprint = 0.0f;
    // Texture coordinate units per world unit on the triangle, for each UV set.
    float uvDensity0 = 0.0f;
    float uvDensity1 = 0.0f;
    // MaterialPreviewTexture::Sample's dither for this sample: a fresh number in [0, 1) per sample
    // of a pixel; negative blends the mip levels.
    float lodDither = -1.0f;
};

// The point at barycentrics (u, v) of the mesh's triangle, met by a ray at distance t. pixelSpread:
// the angle a pixel subtends, radians (0: no level of detail, the finest level).
MaterialPreviewSurfacePoint InterpolateMaterialPreviewPoint(
    const MeshData& mesh, uint32_t triangle, float u, float v, const Ray& ray, float t, float pixelSpread);

// The base colour's alpha at the point (gbuffer.frag's albedo.a), which Mask and Blend cover by.
float MaterialPreviewAlpha(const MaterialPreviewMaterial& material, const MaterialPreviewSurfacePoint& point);

// The surface as the G-buffer holds it and the lighting pass reads it back (deferred_lighting.frag):
// every layer's parameters, floored as the lighting pass floors them.
struct MaterialPreviewSurface
{
    glm::vec3 albedo{1.0f};
    float alpha = 1.0f;
    // The shading normal and the geometric (vertex) normal, both facing the viewer's side.
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec3 geometricNormal{0.0f, 1.0f, 0.0f};
    float metallic = 0.0f;
    float roughness = 1.0f;
    float occlusion = 1.0f;
    // In the preview's unit (kMaterialPreviewNitsPerUnit).
    glm::vec3 emissive{0.0f};
    uint32_t flags = 0;
    float coatFactor = 0.0f;
    float coatRoughness = 1.0f;
    glm::vec3 coatNormal{0.0f, 1.0f, 0.0f};
    glm::vec3 sheenColor{0.0f};
    float sheenRoughness = 1.0f;
    glm::vec3 anisotropyTangent{1.0f, 0.0f, 0.0f};
    float anisotropyStrength = 0.0f;
    glm::vec3 dielectricF0{0.04f};
    float dielectricF90 = 1.0f;
};

MaterialPreviewSurface EvaluateMaterialPreviewSurface(const MaterialPreviewMaterial& material, const MaterialPreviewSurfacePoint& point);

// The light a pixel is shaded with: the environment and its key light, turned as the user turned it.
struct MaterialPreviewLighting
{
    const MaterialPreviewEnvironment* environment = nullptr;
    // Toward the light, unit length.
    glm::vec3 keyDirection{0.0f, 1.0f, 0.0f};
    glm::vec3 keyIlluminance{0.0f};
    // The sine of its angular radius.
    float keySize = 0.0f;
};

// The radiance the surface sends toward the viewer (view: unit, from the surface to the eye), as
// pbr_common.slang's ShadeSurface: the key light through EvaluateBRDF and the coat and sheen lobes,
// times keyVisibility (its shadow ray); the environment's split-sum ambient, its diffuse darkened by
// the material's occlusion times ambientVisibility (the traced occlusion), its specular through
// SpecularOcclusion; then the sheen and coat layered over the base, and the emission.
glm::vec3 ShadeMaterialPreviewSurface(
    const MaterialPreviewSurface& surface,
    const glm::vec3& view,
    const MaterialPreviewLighting& lighting,
    float keyVisibility,
    float ambientVisibility);

// GT7's SDR tone mapping (the viewport's), frame-buffer units (linear Rec.709) in, display-referred
// linear Rec.709 in [0, 1] out. Some two thousand cycles a pixel: the renderer maps only the passes
// it shows.
glm::vec3 MaterialPreviewToneMap(const glm::vec3& frameBuffer);
// The frame-buffer value of the display's paper white, where Unlit materials show their colour.
inline constexpr float kMaterialPreviewPaperWhite = 2.5f;
}
