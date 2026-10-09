#include "material_preview_shading.h"

#include <engine/core/threading/task_system.h>
#include <engine/renderer/environment_brdf.h>
#include <engine/renderer/shader_cpp_compat.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace me
{

namespace
{
// The BRDF terms, the anisotropic lobe, the specular occlusion and the tone mapping the GPU uses,
// compiled from the same sources (the tests compile them so as well).
namespace shader
{
using namespace glm;
using namespace me::shader_cpp;
#include <shaders/vulkan/brdf_common.slang>
#include <shaders/vulkan/anisotropy_common.slang>
#include <shaders/vulkan/ssr_common.slang>
#include <shaders/vulkan/gt7_tonemap.slang>
}

constexpr float kPi = 3.14159265358979f;
constexpr glm::vec3 kCoatF0{0.04f};

// The slots ForEachMaterialTexture gives TextureUsage::Color (sampled through an sRGB format).
constexpr bool IsColorSlot(uint32_t slot)
{
    return slot == 0 || slot == 5 || slot == 6 || slot == 11 || slot == 15 || slot == 19 || slot == 26;
}

// The renderer's default for an unbound slot (UploadSceneResources' defaultSlots).
glm::vec4 DefaultTexel(uint32_t slot)
{
    if (slot == 1 || slot == 7 || slot == 20)
    {
        return glm::vec4(0.5f, 0.5f, 1.0f, 1.0f);
    }
    if (slot == 17)
    {
        return glm::vec4(1.0f, 128.0f / 255.0f, 1.0f, 1.0f);
    }
    return glm::vec4(1.0f);
}

const std::string& SlotPath(const MaterialTexturePaths& paths, uint32_t slot)
{
    static const std::string kEmpty;
    switch (slot)
    {
    case 0: return paths.baseColor;
    case 1: return paths.normal;
    case 2: return paths.metallic;
    case 3: return paths.roughness;
    case 4: return paths.occlusion;
    case 5: return paths.emissive;
    case 6: return paths.secondaryBaseColor;
    case 7: return paths.secondaryNormal;
    case 8: return paths.secondaryMetallic;
    case 9: return paths.secondaryRoughness;
    case 10: return paths.secondaryOcclusion;
    case 11: return paths.secondaryEmissive;
    case 12: return paths.blendMask;
    case 13: return paths.clearcoat;
    case 14: return paths.clearcoatRoughness;
    case 15: return paths.sheenColor;
    case 16: return paths.sheenRoughness;
    case 17: return paths.anisotropy;
    case 18: return paths.specular;
    case 19: return paths.specularColor;
    case 20: return paths.clearcoatNormal;
    case 21: return paths.iridescence;
    case 22: return paths.iridescenceThickness;
    case 23: return paths.transmission;
    case 24: return paths.thickness;
    case 25: return paths.diffuseTransmission;
    case 26: return paths.diffuseTransmissionColor;
    default: return kEmpty;
    }
}

// The DFG table the lighting reads (BuildEnvironmentBrdfLut's layout: A, B and the sheen albedo at
// N.V = (x + 0.5) / size, roughness = (y + 0.5) / size), built once in parallel with fewer samples
// than the renderer's: the preview's noise is in its own sampling, not here.
class EnvironmentBrdfTable
{
  public:
    static const EnvironmentBrdfTable& Get()
    {
        static const EnvironmentBrdfTable table;
        return table;
    }

    // SampleEnvironmentBrdf and SampleSheenAlbedo: bilinear, clamped to texel centres.
    glm::vec3 Sample(float roughness, float NdV) const
    {
        const float size = static_cast<float>(kSize);
        const float x = std::clamp(NdV * size - 0.5f, 0.0f, size - 1.0f);
        const float y = std::clamp(roughness * size - 0.5f, 0.0f, size - 1.0f);
        const int x0 = static_cast<int>(x);
        const int y0 = static_cast<int>(y);
        const int x1 = std::min(x0 + 1, static_cast<int>(kSize) - 1);
        const int y1 = std::min(y0 + 1, static_cast<int>(kSize) - 1);
        const float tx = x - static_cast<float>(x0);
        const float ty = y - static_cast<float>(y0);
        const glm::vec3* const texels = m_texels.data();
        const glm::vec3 top = glm::mix(texels[y0 * kSize + x0], texels[y0 * kSize + x1], tx);
        const glm::vec3 bottom = glm::mix(texels[y1 * kSize + x0], texels[y1 * kSize + x1], tx);
        return glm::mix(top, bottom, ty);
    }

  private:
    static constexpr uint32_t kSize = kEnvironmentBrdfLutSize;
    static constexpr uint32_t kSamples = 512;

    EnvironmentBrdfTable()
        : m_texels(kSize * kSize)
    {
        TaskSystem::ParallelFor(
            kSize,
            1,
            [this](uint32_t begin, uint32_t end)
            {
                for (uint32_t y = begin; y < end; ++y)
                {
                    const float roughness = (static_cast<float>(y) + 0.5f) / static_cast<float>(kSize);
                    for (uint32_t x = 0; x < kSize; ++x)
                    {
                        const float NdV = (static_cast<float>(x) + 0.5f) / static_cast<float>(kSize);
                        glm::vec2 ab = IntegrateEnvironmentBrdf(roughness, NdV, kSamples);
                        const float albedo = ab.x + ab.y;
                        if (albedo > 1.0f)
                        {
                            ab /= albedo;
                        }
                        m_texels[y * kSize + x] = glm::vec3(ab, std::min(IntegrateSheenAlbedo(roughness, NdV, kSamples), 1.0f));
                    }
                }
            },
            TaskPriority::Medium);
    }

    std::vector<glm::vec3> m_texels;
};

// Where a slot's texture is read: its transform's UV set and rows (MaterialSlotUv), or the first
// set as it is; and the texture coordinate units a world unit spans there.
struct SlotCoordinate
{
    glm::vec2 uv{0.0f};
    float density = 0.0f;
};

SlotCoordinate SlotUv(const MaterialPreviewMaterial& material, uint32_t slot, const MaterialPreviewSurfacePoint& point, bool transformed)
{
    if (!transformed || material.material.shadingModel[1] == 0u)
    {
        return {point.uv0, point.uvDensity0};
    }
    const float* const rows = material.transformRows.data()[slot].data();
    const bool second = rows[3] > 0.5f;
    const glm::vec2 base = second ? point.uv1 : point.uv0;
    const float scale = std::sqrt(std::abs(rows[0] * rows[5] - rows[1] * rows[4]));
    return {glm::vec2(rows[0] * base.x + rows[1] * base.y + rows[2], rows[4] * base.x + rows[5] * base.y + rows[6]),
            (second ? point.uvDensity1 : point.uvDensity0) * scale};
}

float TextureLod(const MaterialPreviewTexture& texture, const MaterialPreviewSurfacePoint& point, float density)
{
    const float texels = point.footprint * density * static_cast<float>(std::max(texture.Width(), texture.Height()));
    return texels > 0.0f ? std::log2(texels) : 0.0f;
}

glm::vec4 SampleSlot(const MaterialPreviewMaterial& material, uint32_t slot, const MaterialPreviewSurfacePoint& point, bool transformed = true)
{
    const MaterialPreviewTexture* const texture = material.textures.data()[slot].get();
    if (texture == nullptr || texture->levels.empty())
    {
        return DefaultTexel(slot);
    }
    const SlotCoordinate coordinate = SlotUv(material, slot, point, transformed);
    return texture->Sample(
        coordinate.uv, TextureLod(*texture, point, coordinate.density), material.samplers.data()[slot], IsColorSlot(slot), point.lodDither);
}

// RotateMaterialTangentXy: a tangent-space vector read through a rotated texture, turned back.
glm::vec2 RotateTangentXy(const MaterialPreviewMaterial& material, uint32_t slot, glm::vec2 xy)
{
    if (material.material.shadingModel[1] == 0u)
    {
        return xy;
    }
    const float* const rows = material.transformRows.data()[slot].data();
    glm::vec2 cs(rows[0], -rows[4]);
    const float scale = glm::length(cs);
    if (scale < 1e-8f)
    {
        return xy;
    }
    cs /= scale;
    return glm::vec2(cs.x * xy.x - cs.y * xy.y, cs.y * xy.x + cs.x * xy.y);
}

glm::vec3 DecodeNormalMap(const glm::vec4& texel)
{
    const glm::vec2 xy = glm::vec2(texel) * 2.0f - 1.0f;
    return glm::vec3(xy, std::sqrt(std::max(1.0f - glm::dot(xy, xy), 0.0f)));
}

// DetailLayersFactor (detail_layers.slang).
glm::vec3 DetailLayersFactor(const MaterialPreviewMaterial& material, const MaterialPreviewSurfacePoint& point)
{
    const uint32_t mapping = material.material.shadingModel[2];
    if (mapping == 0u)
    {
        return glm::vec3(1.0f);
    }
    const TextureSampler repeat{};
    const auto sample = [&](const std::shared_ptr<const MaterialPreviewTexture>& texture, glm::vec2 uv, float density)
    {
        if (!texture || texture->levels.empty())
        {
            return glm::vec4(1.0f);
        }
        return texture->Sample(uv, TextureLod(*texture, point, density), repeat, false, point.lodDither);
    };
    const glm::vec2 coordinate = mapping == 2u ? glm::vec2(point.position.x, point.position.z) : point.uv0;
    const float coordinateDensity = mapping == 2u ? 1.0f : point.uvDensity0;
    const glm::vec4 mask = sample(material.detailMask, point.uv0, point.uvDensity0);
    const float* scales = material.material.detailLayerScales;
    glm::vec3 combined(0.0f);
    for (size_t layer = 0; layer < kDetailLayerCount; ++layer)
    {
        const glm::vec2 scale(scales[layer * 2], scales[layer * 2 + 1]);
        const float density = coordinateDensity * std::sqrt(std::abs(scale.x * scale.y));
        combined += glm::vec3(sample(material.detailLayers[layer], coordinate * scale, density)) * mask[static_cast<glm::length_t>(layer)];
    }
    combined *= material.material.detailLayerParams[0];
    combined = glm::max(combined, glm::vec3(0.0f));
    glm::vec3 linear;
    for (glm::length_t channel = 0; channel < 3; ++channel)
    {
        const float encoded = combined[channel];
        linear[channel] = encoded < 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
    }
    return linear;
}

bool HasFlag(uint32_t flags, uint32_t flag)
{
    return (flags & flag) != 0u;
}

glm::vec3 FresnelSchlick(float cosTheta, const glm::vec3& f0, float f90)
{
    return f0 + (glm::vec3(f90) - f0) * std::pow(std::clamp(1.0f - cosTheta, 0.0f, 1.0f), 5.0f);
}

float DistributionGgx(float NdH, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float denominator = std::max(NdH * NdH * (a2 - 1.0f) + 1.0f, 1e-7f);
    return a2 / (kPi * denominator * denominator);
}

float IsotropicSpecularTerm(const glm::vec3& N, const glm::vec3& V, const glm::vec3& L, const glm::vec3& H, float roughness)
{
    return DistributionGgx(std::max(glm::dot(N, H), 0.0f), roughness) *
           shader::VisibilitySmithGgxCorrelated(std::max(glm::dot(N, V), 1e-4f), std::max(glm::dot(N, L), 0.0f), roughness * roughness);
}

float AnisotropicSpecularTerm(const glm::vec3& N, const glm::vec3& V, const glm::vec3& L, const glm::vec3& H, float roughness, const glm::vec3& T, float strength)
{
    const glm::vec3 B = glm::cross(N, T);
    const float alpha = roughness * roughness;
    const float alphaT = glm::mix(alpha, 1.0f, strength * strength);
    const float D = shader::DistributionGgxAnisotropic(std::max(glm::dot(N, H), 0.0f), glm::dot(T, H), glm::dot(B, H), alphaT, alpha);
    const float visibility = shader::VisibilityGgxAnisotropic(
        std::max(glm::dot(N, L), 0.0f), std::max(glm::dot(N, V), 1e-4f), glm::dot(T, V), glm::dot(B, V), glm::dot(T, L), glm::dot(B, L), alphaT, alpha);
    return D * visibility;
}

// SpecularEnergyCompensation (pbr_common.slang).
glm::vec3 EnergyCompensation(const glm::vec3& f0, const glm::vec2& ab)
{
    const float lost = std::clamp(1.0f - (ab.x + ab.y), 0.0f, 1.0f - 1e-4f);
    const glm::vec3 average = f0 + (glm::vec3(1.0f) - f0) / 21.0f;
    return glm::vec3(1.0f) + lost * average / (glm::vec3(1.0f) - average * lost);
}

// EvaluateBaseSpecularAlbedos: the base's split-sum albedo with its multiple scattering, total and
// the dielectric share's.
void BaseSpecularAlbedos(const glm::vec2& ab, const MaterialPreviewSurface& surface, glm::vec3& total, glm::vec3& dielectric)
{
    dielectric = (surface.dielectricF0 * ab.x + surface.dielectricF90 * ab.y) * EnergyCompensation(surface.dielectricF0, ab);
    const glm::vec3 metal = surface.metallic > 0.0f ? (surface.albedo * ab.x + ab.y) * EnergyCompensation(surface.albedo, ab) : glm::vec3(0.0f);
    total = glm::mix(dielectric, metal, surface.metallic);
}

// SpecularLightDirection for a directional light: the disk's point nearest the reflection vector,
// and the factor the lobe's peak falls by.
glm::vec3 DiskSpecularDirection(const glm::vec3& L, float size, const glm::vec3& N, const glm::vec3& V, float roughness, float& normalization)
{
    normalization = 1.0f;
    if (size <= 0.0f)
    {
        return L;
    }
    const glm::vec3 R = glm::reflect(-V, N);
    normalization = shader::SourceSizeNormalization(roughness * roughness, size);
    return shader::DiskLightSpecularDirection(L, R, std::sqrt(std::max(1.0f - size * size, 0.0f)), size);
}
}

std::vector<std::string> MaterialPreviewTexturePaths(const CpuRenderSubmesh& submesh)
{
    std::vector<std::string> paths;
    for (uint32_t slot = 0; slot < kMaterialTextureSlotCount; ++slot)
    {
        if (!SlotPath(submesh.textures, slot).empty())
        {
            paths.push_back(SlotPath(submesh.textures, slot));
        }
    }
    if (submesh.material.shadingModel[2] != 0u)
    {
        paths.push_back(submesh.textures.detailMask);
        for (const std::string& layer : submesh.textures.detailLayers)
        {
            paths.push_back(layer);
        }
    }
    return paths;
}

MaterialPreviewMaterial BuildMaterialPreviewMaterial(const CpuRenderSubmesh& submesh, const MaterialPreviewTextureCache& textures)
{
    MaterialPreviewMaterial material;
    material.material = submesh.material;
    material.alphaMode = submesh.alphaMode;
    material.doubleSided = submesh.doubleSided;
    material.samplers = submesh.textureSamplers;
    for (uint32_t slot = 0; slot < kMaterialTextureSlotCount; ++slot)
    {
        float row0[4];
        float row1[4];
        ComputeTextureTransformRows(submesh.textureTransforms[slot], row0, row1);
        for (size_t index = 0; index < 4; ++index)
        {
            material.transformRows[slot][index] = row0[index];
            material.transformRows[slot][index + 4] = row1[index];
        }
        const std::string& path = SlotPath(submesh.textures, slot);
        if (!path.empty())
        {
            material.textures[slot] = textures.Find(path);
        }
    }
    if (submesh.material.shadingModel[2] != 0u)
    {
        material.detailMask = textures.Find(submesh.textures.detailMask);
        for (size_t layer = 0; layer < kDetailLayerCount; ++layer)
        {
            if (!submesh.textures.detailLayers[layer].empty())
            {
                material.detailLayers[layer] = textures.Find(submesh.textures.detailLayers[layer]);
            }
        }
    }
    return material;
}

MaterialPreviewSurfacePoint InterpolateMaterialPreviewPoint(
    const MeshData& mesh, uint32_t triangle, float u, float v, const Ray& ray, float t, float pixelSpread)
{
    MaterialPreviewSurfacePoint point;
    // Raw pointers: a Debug build's checked indexing doubles the cost of the hot loops.
    const uint32_t* const indices = mesh.indices.data() + 3u * triangle;
    const Vertex* const vertices = mesh.vertices.data();
    const Vertex& a = vertices[indices[0]];
    const Vertex& b = vertices[indices[1]];
    const Vertex& c = vertices[indices[2]];
    const float w = 1.0f - u - v;
    const auto vec2Of = [](const float* values)
    {
        return glm::vec2(values[0], values[1]);
    };
    const auto vec3Of = [](const float* values)
    {
        return glm::vec3(values[0], values[1], values[2]);
    };
    const glm::vec3 pa = vec3Of(a.position);
    const glm::vec3 pb = vec3Of(b.position);
    const glm::vec3 pc = vec3Of(c.position);
    point.position = pa * w + pb * u + pc * v;
    const glm::vec3 cross = glm::cross(pb - pa, pc - pa);
    const float doubleArea = glm::length(cross);
    point.faceNormal = doubleArea > 0.0f ? cross / doubleArea : glm::vec3(0.0f, 1.0f, 0.0f);
    point.frontFacing = glm::dot(point.faceNormal, ray.direction) < 0.0f;
    point.vertexNormal = vec3Of(a.normal) * w + vec3Of(b.normal) * u + vec3Of(c.normal) * v;
    if (glm::dot(point.vertexNormal, point.vertexNormal) < 1e-12f)
    {
        point.vertexNormal = point.faceNormal;
    }
    point.vertexTangent = glm::vec4(a.tangent[0], a.tangent[1], a.tangent[2], a.tangent[3]) * w +
                          glm::vec4(b.tangent[0], b.tangent[1], b.tangent[2], b.tangent[3]) * u +
                          glm::vec4(c.tangent[0], c.tangent[1], c.tangent[2], c.tangent[3]) * v;
    point.vertexTangent.w = a.tangent[3] < 0.0f ? -1.0f : 1.0f;
    const glm::vec2 ta = vec2Of(a.texCoord);
    const glm::vec2 tb = vec2Of(b.texCoord);
    const glm::vec2 tc = vec2Of(c.texCoord);
    point.uv0 = ta * w + tb * u + tc * v;
    const glm::vec2 sa = vec2Of(a.texCoord1);
    const glm::vec2 sb = vec2Of(b.texCoord1);
    const glm::vec2 sc = vec2Of(c.texCoord1);
    point.uv1 = sa * w + sb * u + sc * v;
    point.color = vec3Of(a.color) * w + vec3Of(b.color) * u + vec3Of(c.color) * v;
    if (doubleArea > 0.0f)
    {
        const auto uvArea = [](const glm::vec2& p, const glm::vec2& q, const glm::vec2& r)
        {
            const glm::vec2 e1 = q - p;
            const glm::vec2 e2 = r - p;
            return std::abs(e1.x * e2.y - e1.y * e2.x);
        };
        point.uvDensity0 = std::sqrt(uvArea(ta, tb, tc) / doubleArea);
        point.uvDensity1 = std::sqrt(uvArea(sa, sb, sc) / doubleArea);
    }
    const float cosine = std::abs(glm::dot(point.faceNormal, ray.direction));
    point.footprint = pixelSpread * t / std::max(cosine, 0.15f);
    return point;
}

float MaterialPreviewAlpha(const MaterialPreviewMaterial& material, const MaterialPreviewSurfacePoint& point)
{
    const GpuMaterialData& gpu = material.material;
    const float blendMask = SampleSlot(material, 12, point, false).r;
    const float blendWeight = std::clamp(glm::mix(0.0f, gpu.nodeGraphFactors[1], std::clamp(gpu.nodeGraphFactors[0], 0.0f, 1.0f)) * blendMask, 0.0f, 1.0f);
    const float primary = SampleSlot(material, 0, point).a;
    const float secondary = blendWeight > 0.0f ? SampleSlot(material, 6, point, false).a : primary;
    return glm::mix(primary, secondary, blendWeight) * gpu.baseColorFactor[3];
}

MaterialPreviewSurface EvaluateMaterialPreviewSurface(const MaterialPreviewMaterial& material, const MaterialPreviewSurfacePoint& point)
{
    const GpuMaterialData& gpu = material.material;
    MaterialPreviewSurface surface;

    // Blend mask and weight, then the albedo (gbuffer.frag).
    const float blendMask = SampleSlot(material, 12, point, false).r;
    const float blendWeight = std::clamp(glm::mix(0.0f, gpu.nodeGraphFactors[1], std::clamp(gpu.nodeGraphFactors[0], 0.0f, 1.0f)) * blendMask, 0.0f, 1.0f);
    const bool blended = blendWeight > 0.0f;
    const auto mixSecondary = [&](uint32_t primarySlot, uint32_t secondarySlot)
    {
        const glm::vec4 primary = SampleSlot(material, primarySlot, point);
        return blended ? glm::mix(primary, SampleSlot(material, secondarySlot, point, false), blendWeight) : primary;
    };
    const glm::vec4 sampledBaseColor = mixSecondary(0, 6);
    glm::vec4 albedo = sampledBaseColor * glm::vec4(point.color, 1.0f) *
                       glm::vec4(gpu.baseColorFactor[0], gpu.baseColorFactor[1], gpu.baseColorFactor[2], gpu.baseColorFactor[3]);
    surface.albedo = glm::vec3(albedo) * DetailLayersFactor(material, point);
    surface.alpha = albedo.a;
    surface.flags = gpu.shadingModel[0];

    // The tangent frame, mirrored on a back face (double-sided), and the normal map.
    const float faceSign = point.frontFacing ? 1.0f : -1.0f;
    const glm::vec3 geoNormal = glm::normalize(point.vertexNormal) * faceSign;
    const glm::vec3 faceTangent = glm::vec3(point.vertexTangent) * faceSign;
    glm::vec3 tangent = faceTangent - geoNormal * glm::dot(geoNormal, faceTangent);
    if (glm::dot(tangent, tangent) < 1e-12f)
    {
        tangent = shader::OrthonormalTangent(geoNormal);
    }
    tangent = glm::normalize(tangent);
    const glm::vec3 bitangent = glm::normalize(glm::cross(geoNormal, tangent) * point.vertexTangent.w) * faceSign;
    const glm::mat3 tbn(tangent, bitangent, geoNormal);
    glm::vec3 normalPrimary = DecodeNormalMap(SampleSlot(material, 1, point));
    const glm::vec2 rotated = RotateTangentXy(material, 1, glm::vec2(normalPrimary));
    normalPrimary.x = rotated.x;
    normalPrimary.y = rotated.y;
    glm::vec3 normalSample = blended ? glm::normalize(glm::mix(normalPrimary, DecodeNormalMap(SampleSlot(material, 7, point, false)), blendWeight))
                                     : glm::normalize(normalPrimary);
    normalSample.x *= gpu.surfaceFactors[2];
    normalSample.y *= gpu.surfaceFactors[2];
    surface.normal = glm::normalize(tbn * normalSample);
    surface.geometricNormal = geoNormal;

    surface.metallic = std::clamp(gpu.surfaceFactors[0] * mixSecondary(2, 8).b, 0.0f, 1.0f);
    surface.roughness = std::clamp(gpu.surfaceFactors[1] * mixSecondary(3, 9).g, 0.04f, 1.0f);
    surface.occlusion = glm::mix(1.0f, mixSecondary(4, 10).r, std::clamp(gpu.surfaceFactors[3], 0.0f, 1.0f));
    surface.emissive = glm::vec3(mixSecondary(5, 11)) * glm::vec3(gpu.emissiveFactor[0], gpu.emissiveFactor[1], gpu.emissiveFactor[2]) *
                       (1.0f / kMaterialPreviewNitsPerUnit);

    // The layers (EvaluateMaterialLayers), as the lighting pass reads them back.
    surface.coatNormal = geoNormal;
    if (HasFlag(surface.flags, kShadingFlagClearcoat))
    {
        surface.coatFactor = std::clamp(gpu.clearcoatFactors[0] * SampleSlot(material, 13, point).r, 0.0f, 1.0f);
        surface.coatRoughness = std::clamp(gpu.clearcoatFactors[1] * SampleSlot(material, 14, point).g, 0.04f, 1.0f);
        if (HasFlag(surface.flags, kShadingFlagCoatNormal))
        {
            glm::vec3 coatSample = DecodeNormalMap(SampleSlot(material, 20, point));
            const glm::vec2 coatRotated = RotateTangentXy(material, 20, glm::vec2(coatSample)) * gpu.clearcoatFactors[2];
            coatSample.x = coatRotated.x;
            coatSample.y = coatRotated.y;
            surface.coatNormal = glm::normalize(tbn * coatSample);
        }
    }
    if (HasFlag(surface.flags, kShadingFlagSheen))
    {
        surface.sheenColor = glm::clamp(
            glm::vec3(gpu.sheenFactors[0], gpu.sheenFactors[1], gpu.sheenFactors[2]) * glm::vec3(SampleSlot(material, 15, point)), 0.0f, 1.0f);
        surface.sheenRoughness = std::clamp(gpu.sheenFactors[3] * SampleSlot(material, 16, point).a, 0.04f, 1.0f);
    }
    surface.anisotropyTangent = tangent;
    if (HasFlag(surface.flags, kShadingFlagAnisotropy))
    {
        const glm::vec3 sampled(SampleSlot(material, 17, point));
        const glm::vec2 direction = RotateTangentXy(
            material, 17, shader::AnisotropyDirection(glm::vec2(sampled), gpu.anisotropyFactors[1], gpu.anisotropyFactors[2]));
        glm::vec3 anisotropyTangent = tbn * glm::vec3(direction, 0.0f);
        anisotropyTangent -= surface.normal * glm::dot(surface.normal, anisotropyTangent);
        const float length2 = glm::dot(anisotropyTangent, anisotropyTangent);
        if (length2 > 1e-8f)
        {
            surface.anisotropyTangent = anisotropyTangent / std::sqrt(length2);
            surface.anisotropyStrength = std::clamp(gpu.anisotropyFactors[0] * sampled.b, 0.0f, 1.0f);
        }
    }
    if (HasFlag(surface.flags, kShadingFlagSpecular))
    {
        const float specular = std::clamp(gpu.specularFactors[3] * SampleSlot(material, 18, point).a, 0.0f, 1.0f);
        surface.dielectricF0 = glm::min(
                                   glm::vec3(gpu.specularFactors[0], gpu.specularFactors[1], gpu.specularFactors[2]) *
                                       glm::vec3(SampleSlot(material, 19, point)),
                                   glm::vec3(1.0f)) *
                               specular;
        surface.dielectricF90 = specular;
    }
    return surface;
}

glm::vec3 ShadeMaterialPreviewSurface(
    const MaterialPreviewSurface& surface,
    const glm::vec3& view,
    const MaterialPreviewLighting& lighting,
    float keyVisibility,
    float ambientVisibility)
{
    if (HasFlag(surface.flags, kShadingFlagUnlit))
    {
        // In frame-buffer units the caller divides by its exposure: paper white at any exposure.
        return surface.albedo * kMaterialPreviewPaperWhite;
    }
    const MaterialPreviewEnvironment& environment = *lighting.environment;
    const EnvironmentBrdfTable& table = EnvironmentBrdfTable::Get();
    const glm::vec3& N = surface.normal;
    const glm::vec3& V = view;
    const float NdV = std::max(glm::dot(N, V), 0.0f);
    const float ao = surface.occlusion * ambientVisibility;
    const bool anisotropic = surface.anisotropyStrength > 0.0f;

    // The ambient term (EvaluateSkyAmbient).
    const glm::vec3 dfg = table.Sample(surface.roughness, NdV);
    const glm::vec2 ab(dfg.x, dfg.y);
    glm::vec3 specularAlbedo;
    glm::vec3 dielectricAlbedo;
    BaseSpecularAlbedos(ab, surface, specularAlbedo, dielectricAlbedo);
    const glm::vec3 diffuseAlbedo = surface.albedo * (1.0f - surface.metallic) * (glm::vec3(1.0f) - dielectricAlbedo);
    const glm::vec3 reflected = glm::reflect(
        -V, anisotropic ? shader::AnisotropicBentNormal(N, V, surface.anisotropyTangent, surface.anisotropyStrength, surface.roughness) : N);
    const glm::vec3 diffuseAmbient = diffuseAlbedo * environment.DiffuseRadiance(N) * ao;
    const glm::vec3 specularAmbient = specularAlbedo * environment.Specular(reflected, surface.roughness) *
                                      (shader::SpecularOcclusion(NdV, ao, surface.roughness) *
                                       shader::HorizonSpecularOcclusion(reflected, surface.geometricNormal));
    const glm::vec3 ambient = diffuseAmbient + specularAmbient;

    // The key light (EvaluateSceneLight for a directional light).
    const glm::vec3 f0 = glm::mix(surface.dielectricF0, surface.albedo, surface.metallic);
    const float f90 = glm::mix(surface.dielectricF90, 1.0f, surface.metallic);
    const glm::vec3 uncompensated = f0 * ab.x + f90 * ab.y;
    const glm::vec3 energyCompensation = specularAlbedo / glm::max(uncompensated, glm::vec3(1e-4f));
    const glm::vec3& L = lighting.keyDirection;
    const glm::vec3 radiance = lighting.keyIlluminance * keyVisibility;
    glm::vec3 direct(0.0f);
    glm::vec3 coatDirect(0.0f);
    glm::vec3 sheenDirect(0.0f);
    if (keyVisibility > 0.0f)
    {
        const float NdL = std::max(glm::dot(N, L), 0.0f);
        if (NdL > 0.0f)
        {
            const glm::vec3 H = glm::normalize(V + L);
            const glm::vec3 kD = (glm::vec3(1.0f) - FresnelSchlick(std::max(glm::dot(H, V), 0.0f), surface.dielectricF0, surface.dielectricF90)) *
                                 (1.0f - surface.metallic);
            direct += kD * surface.albedo * shader::BurleyDiffuse(std::max(NdV, 1e-4f), NdL, std::max(glm::dot(L, H), 0.0f), surface.roughness) *
                      radiance * NdL;
        }
        float normalization = 1.0f;
        const glm::vec3 specularL = DiskSpecularDirection(L, lighting.keySize, N, V, surface.roughness, normalization);
        const float specularNdL = std::max(glm::dot(N, specularL), 0.0f);
        if (specularNdL > 0.0f)
        {
            const glm::vec3 H = glm::normalize(V + specularL);
            const glm::vec3 F = FresnelSchlick(std::max(glm::dot(H, V), 0.0f), f0, f90);
            const float term = anisotropic
                                   ? AnisotropicSpecularTerm(N, V, specularL, H, surface.roughness, surface.anisotropyTangent, surface.anisotropyStrength)
                                   : IsotropicSpecularTerm(N, V, specularL, H, surface.roughness);
            direct += term * normalization * F * energyCompensation * radiance * specularNdL;
        }
        if (surface.coatFactor > 0.0f)
        {
            float coatNormalization = 1.0f;
            const glm::vec3 coatL = DiskSpecularDirection(L, lighting.keySize, surface.coatNormal, V, surface.coatRoughness, coatNormalization);
            const float coatNdL = std::max(glm::dot(surface.coatNormal, coatL), 0.0f);
            if (coatNdL > 0.0f)
            {
                const glm::vec3 H = glm::normalize(V + coatL);
                const glm::vec3 F = FresnelSchlick(std::max(glm::dot(H, V), 0.0f), kCoatF0, 1.0f);
                coatDirect = IsotropicSpecularTerm(surface.coatNormal, V, coatL, H, surface.coatRoughness) * coatNormalization * F * radiance * coatNdL;
            }
        }
        const bool sheen = std::max({surface.sheenColor.r, surface.sheenColor.g, surface.sheenColor.b}) > 0.0f;
        if (sheen)
        {
            const float NdL = std::max(glm::dot(N, L), 0.0f);
            if (NdL > 0.0f)
            {
                const glm::vec3 H = glm::normalize(V + L);
                const float alpha = surface.sheenRoughness * surface.sheenRoughness;
                const float NdH = std::max(glm::dot(N, H), 0.0f);
                const float sin2h = std::max(1.0f - NdH * NdH, 0.0078125f);
                const float D = (2.0f + 1.0f / alpha) * std::pow(sin2h, 0.5f / alpha) / (2.0f * kPi);
                const float visibility = 1.0f / (4.0f * std::max(NdL + NdV - NdL * NdV, 1e-4f));
                sheenDirect = surface.sheenColor * (D * visibility) * radiance * NdL;
            }
        }
    }

    // The layers over the base (ShadeSurface's composition).
    glm::vec3 color;
    if (std::max({surface.sheenColor.r, surface.sheenColor.g, surface.sheenColor.b}) > 0.0f)
    {
        const float sheenAlbedo = table.Sample(surface.sheenRoughness, NdV).z;
        const float sheenScaling = 1.0f - std::max({surface.sheenColor.r, surface.sheenColor.g, surface.sheenColor.b}) * sheenAlbedo;
        const glm::vec3 sheenAmbient = surface.sheenColor * sheenAlbedo * environment.Specular(glm::reflect(-V, N), surface.sheenRoughness) * ao;
        color = (ambient + direct) * sheenScaling + sheenDirect + sheenAmbient + surface.emissive;
    }
    else
    {
        color = ambient + direct + surface.emissive;
    }
    if (surface.coatFactor > 0.0f)
    {
        const float coatNdV = std::max(glm::dot(surface.coatNormal, V), 0.0f);
        const glm::vec3 coatTable = table.Sample(surface.coatRoughness, coatNdV);
        const glm::vec3 coatAlbedo = kCoatF0 * coatTable.x + coatTable.y;
        const glm::vec3 coatR = glm::reflect(-V, surface.coatNormal);
        const glm::vec3 coatAmbient = coatAlbedo * environment.Specular(coatR, surface.coatRoughness) *
                                      (shader::SpecularOcclusion(coatNdV, ao, surface.coatRoughness) *
                                       shader::HorizonSpecularOcclusion(coatR, surface.geometricNormal));
        const float coatFresnel = FresnelSchlick(coatNdV, kCoatF0, 1.0f).x;
        color = color * (1.0f - surface.coatFactor * coatFresnel) + surface.coatFactor * (coatAmbient + coatDirect);
    }
    return color;
}

glm::vec3 MaterialPreviewToneMap(const glm::vec3& frameBuffer)
{
    return shader::TonemapFrameBufferRec709(frameBuffer);
}
}
