#include "editor_model_preview.h"
#include "editor_ui_internal.h"

#include <material_graph_runtime.h>
#include <model_loader.h>
#include <texture_loader.h>

#include <editor_world.h>
#include <file_dialog/file_dialog.h>
#include <log/log.h>
#include <ui/ui_scale.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <yaml-cpp/yaml.h>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/common.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace
{
constexpr size_t kMaxMaterialPreviewTriangles = 12000;
constexpr size_t kMaxUvPreviewTriangles = 2400;

glm::vec3 ComputeLoadedModelCenter(const LoadedModelData& loadedModel)
{
    if (loadedModel.hasBounds)
    {
        return (loadedModel.minBounds + loadedModel.maxBounds) * 0.5f;
    }

    glm::vec3 minBounds(FLT_MAX);
    glm::vec3 maxBounds(-FLT_MAX);
    bool hasVertex = false;
    for (const ModelSubmeshData& submesh : loadedModel.submeshes)
    {
        for (const Vertex& vertex : submesh.mesh.vertices)
        {
            const glm::vec3 position(vertex.position[0], vertex.position[1], vertex.position[2]);
            minBounds = glm::min(minBounds, position);
            maxBounds = glm::max(maxBounds, position);
            hasVertex = true;
        }
    }

    return hasVertex ? (minBounds + maxBounds) * 0.5f : glm::vec3(0.0f);
}

float ComputeLoadedModelRadius(const LoadedModelData& loadedModel, const glm::vec3& center)
{
    if (loadedModel.hasBounds)
    {
        return std::max(glm::length(loadedModel.maxBounds - center), 0.25f);
    }

    float radius = 0.25f;
    for (const ModelSubmeshData& submesh : loadedModel.submeshes)
    {
        for (const Vertex& vertex : submesh.mesh.vertices)
        {
            const glm::vec3 position(vertex.position[0], vertex.position[1], vertex.position[2]);
            radius = std::max(radius, glm::length(position - center));
        }
    }
    return radius;
}

std::optional<ImVec2> ProjectPreviewPoint(
    const glm::vec3& position,
    const glm::mat4& viewProjection,
    const ImVec2& canvasMin,
    const ImVec2& canvasSize)
{
    const glm::vec4 clip = viewProjection * glm::vec4(position, 1.0f);
    if (clip.w <= 0.001f)
    {
        return std::nullopt;
    }

    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    const float screenX = canvasMin.x + (ndc.x * 0.5f + 0.5f) * canvasSize.x;
    const float screenY = canvasMin.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * canvasSize.y;
    return ImVec2(screenX, screenY);
}
}

void DrawModelUvPreview(
    const LoadedModelData& loadedModel,
    int& selectedUvSubmeshIndex,
    float uiScale)
{
    std::vector<size_t> uvSubmeshIndices;
    uvSubmeshIndices.reserve(loadedModel.submeshes.size());
    for (size_t submeshIndex = 0; submeshIndex < loadedModel.submeshes.size(); ++submeshIndex)
    {
        if (loadedModel.submeshes[submeshIndex].hasTexCoords)
        {
            uvSubmeshIndices.push_back(submeshIndex);
        }
    }

    if (uvSubmeshIndices.empty())
    {
        ImGui::TextDisabled("This model does not contain any UV-capable submeshes.");
        return;
    }

    selectedUvSubmeshIndex = std::clamp(selectedUvSubmeshIndex, 0, static_cast<int>(uvSubmeshIndices.size()) - 1);
    const auto buildUvLabel = [&loadedModel, &uvSubmeshIndices](size_t uvListIndex)
    {
        const size_t submeshIndex = uvSubmeshIndices[uvListIndex];
        const ModelSubmeshData& submesh = loadedModel.submeshes[submeshIndex];
        const std::string name = submesh.name.empty()
                                     ? ("Submesh " + std::to_string(submeshIndex))
                                     : submesh.name;
        return name + "##uv_" + std::to_string(submeshIndex);
    };

    const std::string currentUvLabel = buildUvLabel(static_cast<size_t>(selectedUvSubmeshIndex));
    if (ImGui::BeginCombo("UV Submesh", currentUvLabel.c_str()))
    {
        for (size_t uvListIndex = 0; uvListIndex < uvSubmeshIndices.size(); ++uvListIndex)
        {
            const bool isSelected = static_cast<int>(uvListIndex) == selectedUvSubmeshIndex;
            const std::string label = buildUvLabel(uvListIndex);
            if (ImGui::Selectable(label.c_str(), isSelected))
            {
                selectedUvSubmeshIndex = static_cast<int>(uvListIndex);
            }
            if (isSelected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    const ModelSubmeshData& submesh = loadedModel.submeshes[uvSubmeshIndices[static_cast<size_t>(selectedUvSubmeshIndex)]];
    const size_t triangleCount = submesh.mesh.indices.size() / 3u;
    const size_t triangleStep =
        std::max<size_t>(1u, (triangleCount + kMaxUvPreviewTriangles - 1u) / kMaxUvPreviewTriangles);
    const bool cappedUvPreview = triangleStep > 1u;
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float canvasEdge = std::max(220.0f * uiScale, std::min(available.x, 360.0f * uiScale));
    const ImVec2 canvasSize(canvasEdge, canvasEdge);

    ImGui::InvisibleButton("UvPreviewCanvas", canvasSize);
    const ImVec2 canvasMin = ImGui::GetItemRectMin();
    const ImVec2 canvasMax = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(canvasMin, canvasMax, IM_COL32(15, 17, 24, 255), 10.0f * uiScale);
    drawList->AddRect(canvasMin, canvasMax, IM_COL32(94, 104, 126, 255), 10.0f * uiScale, 0, 1.25f);

    const float padding = 20.0f * uiScale;
    const ImVec2 uvMin(canvasMin.x + padding, canvasMin.y + padding);
    const ImVec2 uvMax(canvasMax.x - padding, canvasMax.y - padding);
    const ImU32 gridColor = IM_COL32(54, 64, 84, 255);
    for (int step = 0; step <= 4; ++step)
    {
        const float t = static_cast<float>(step) / 4.0f;
        const float x = uvMin.x + (uvMax.x - uvMin.x) * t;
        const float y = uvMin.y + (uvMax.y - uvMin.y) * t;
        drawList->AddLine(ImVec2(x, uvMin.y), ImVec2(x, uvMax.y), gridColor, 1.0f);
        drawList->AddLine(ImVec2(uvMin.x, y), ImVec2(uvMax.x, y), gridColor, 1.0f);
    }
    drawList->AddRect(uvMin, uvMax, IM_COL32(130, 146, 176, 255), 0.0f, 0, 1.3f);

    const auto mapUv = [&uvMin, &uvMax](const Vertex& vertex)
    {
        const float x = uvMin.x + vertex.texCoord[0] * (uvMax.x - uvMin.x);
        const float y = uvMin.y + vertex.texCoord[1] * (uvMax.y - uvMin.y);
        return ImVec2(x, y);
    };

    for (size_t triangleIndex = 0; triangleIndex < triangleCount; triangleIndex += triangleStep)
    {
        const size_t index = triangleIndex * 3u;
        const uint32_t index0 = submesh.mesh.indices[index];
        const uint32_t index1 = submesh.mesh.indices[index + 1];
        const uint32_t index2 = submesh.mesh.indices[index + 2];
        if (index0 >= submesh.mesh.vertices.size() ||
            index1 >= submesh.mesh.vertices.size() ||
            index2 >= submesh.mesh.vertices.size())
        {
            continue;
        }

        const ImVec2 point0 = mapUv(submesh.mesh.vertices[index0]);
        const ImVec2 point1 = mapUv(submesh.mesh.vertices[index1]);
        const ImVec2 point2 = mapUv(submesh.mesh.vertices[index2]);
        drawList->AddLine(point0, point1, IM_COL32(111, 212, 255, 235), 1.0f);
        drawList->AddLine(point1, point2, IM_COL32(111, 212, 255, 235), 1.0f);
        drawList->AddLine(point2, point0, IM_COL32(111, 212, 255, 235), 1.0f);
    }

    drawList->AddText(
        ImVec2(canvasMin.x + 12.0f * uiScale, canvasMin.y + 12.0f * uiScale),
        IM_COL32(214, 223, 238, 255),
        "UV 0-1 space");
    drawList->AddText(
        ImVec2(canvasMin.x + 12.0f * uiScale, canvasMax.y - 24.0f * uiScale),
        IM_COL32(170, 182, 204, 255),
        cappedUvPreview ? "Sampled wireframe preview for responsiveness" : "Full UV wireframe preview");
}

namespace
{
struct CachedPreviewTexture
{
    TextureData texture;
    std::filesystem::file_time_type lastWriteTime{};
    bool resolved = false;
    bool available = false;
};

struct PreviewSurfaceVertex
{
    glm::vec3 position{0.0f};
    glm::vec3 color{1.0f};
    glm::vec2 uv{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
};

struct PreviewRasterVertex
{
    ImVec2 screen{0.0f, 0.0f};
    float depth = 0.0f;
    ImU32 color = IM_COL32_WHITE;
    bool visible = false;
};

struct PreviewTriangle
{
    PreviewRasterVertex vertices[3];
    float depth = 0.0f;
};

struct MaterialShadedPreviewCache
{
    int canvasOriginX = 0;
    int canvasOriginY = 0;
    int canvasWidth = 0;
    int canvasHeight = 0;
    int selectedMaterialIndex = -1;
    int yawMilliDegrees = 0;
    int pitchMilliDegrees = 0;
    int distanceMilliUnits = 0;
    size_t materialSignature = 0;
    std::vector<PreviewTriangle> triangles;
    bool cappedTriangles = false;
    bool valid = false;
};

std::unordered_map<std::string, CachedPreviewTexture>& GetPreviewTextureCache()
{
    static std::unordered_map<std::string, CachedPreviewTexture> cache;
    return cache;
}

std::unordered_map<std::string, MaterialShadedPreviewCache>& GetMaterialShadedPreviewCaches()
{
    static std::unordered_map<std::string, MaterialShadedPreviewCache> caches;
    return caches;
}
}

void ResetMaterialShadedPreviewCache(const char* canvasId)
{
    if (canvasId == nullptr)
    {
        return;
    }

    GetMaterialShadedPreviewCaches().erase(canvasId);
}

namespace
{
template <typename TValue>
void HashCombine(size_t& seed, const TValue& value)
{
    seed ^= std::hash<TValue>{}(value) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
}

void HashQuantizedFloat(size_t& seed, float value, float scale = 1000.0f)
{
    HashCombine(seed, static_cast<int>(std::lround(value * scale)));
}

void HashPreviewTexturePath(size_t& seed, const std::string& texturePath)
{
    HashCombine(seed, texturePath);
}

void HashPreviewPbrSurfaceSettings(size_t& seed, const MaterialPbrSurfaceSettings& pbr)
{
    for (float component : pbr.baseColorFactor)
    {
        HashQuantizedFloat(seed, component);
    }
    for (float component : pbr.emissiveColor)
    {
        HashQuantizedFloat(seed, component);
    }

    HashQuantizedFloat(seed, pbr.metallicFactor);
    HashQuantizedFloat(seed, pbr.roughnessFactor);
    HashQuantizedFloat(seed, pbr.normalScale);
    HashQuantizedFloat(seed, pbr.occlusionStrength);
    HashQuantizedFloat(seed, pbr.emissiveIntensity);
    HashQuantizedFloat(seed, pbr.opacity);
}

void HashPreviewBlendGraph(size_t& seed, const MaterialTextureBlendGraph& blendGraph)
{
    HashCombine(seed, blendGraph.enabled);
    HashQuantizedFloat(seed, blendGraph.blendFactor);
    HashPreviewTexturePath(seed, blendGraph.blendMaskTexturePath);
    HashPreviewTexturePath(seed, blendGraph.secondaryBaseColorTexturePath);
    HashPreviewTexturePath(seed, blendGraph.secondaryNormalTexturePath);
    HashPreviewTexturePath(seed, blendGraph.secondaryMetallicTexturePath);
    HashPreviewTexturePath(seed, blendGraph.secondaryRoughnessTexturePath);
    HashPreviewTexturePath(seed, blendGraph.secondaryOcclusionTexturePath);
    HashPreviewTexturePath(seed, blendGraph.secondaryEmissiveTexturePath);
}

size_t ComputePreviewMaterialSignature(const std::vector<ModelImportedMaterialInfo>& materials)
{
    size_t signature = 0;
    HashCombine(signature, materials.size());
    for (const ModelImportedMaterialInfo& material : materials)
    {
        HashPreviewTexturePath(signature, material.baseColorTexturePath);
        HashPreviewTexturePath(signature, material.normalTexturePath);
        HashPreviewTexturePath(signature, material.metallicTexturePath);
        HashPreviewTexturePath(signature, material.roughnessTexturePath);
        HashPreviewTexturePath(signature, material.occlusionTexturePath);
        HashPreviewTexturePath(signature, material.emissiveTexturePath);
        HashPreviewPbrSurfaceSettings(signature, material.pbr);
        HashPreviewBlendGraph(signature, material.blendGraph);
    }

    return signature;
}

bool IsMatchingMaterialShadedPreviewCache(
    const MaterialShadedPreviewCache& cache,
    const ImVec2& canvasMin,
    const ImVec2& canvasSize,
    int selectedMaterialIndex,
    float yaw,
    float pitch,
    float distance,
    size_t materialSignature)
{
    return cache.valid &&
           cache.canvasOriginX == static_cast<int>(std::lround(canvasMin.x)) &&
           cache.canvasOriginY == static_cast<int>(std::lround(canvasMin.y)) &&
           cache.canvasWidth == static_cast<int>(std::lround(canvasSize.x)) &&
           cache.canvasHeight == static_cast<int>(std::lround(canvasSize.y)) &&
           cache.selectedMaterialIndex == selectedMaterialIndex &&
           cache.yawMilliDegrees == static_cast<int>(std::lround(yaw * 1000.0f)) &&
           cache.pitchMilliDegrees == static_cast<int>(std::lround(pitch * 1000.0f)) &&
           cache.distanceMilliUnits == static_cast<int>(std::lround(distance * 1000.0f)) &&
           cache.materialSignature == materialSignature;
}

void StoreMaterialShadedPreviewCacheKey(
    MaterialShadedPreviewCache& cache,
    const ImVec2& canvasMin,
    const ImVec2& canvasSize,
    int selectedMaterialIndex,
    float yaw,
    float pitch,
    float distance,
    size_t materialSignature)
{
    cache.canvasOriginX = static_cast<int>(std::lround(canvasMin.x));
    cache.canvasOriginY = static_cast<int>(std::lround(canvasMin.y));
    cache.canvasWidth = static_cast<int>(std::lround(canvasSize.x));
    cache.canvasHeight = static_cast<int>(std::lround(canvasSize.y));
    cache.selectedMaterialIndex = selectedMaterialIndex;
    cache.yawMilliDegrees = static_cast<int>(std::lround(yaw * 1000.0f));
    cache.pitchMilliDegrees = static_cast<int>(std::lround(pitch * 1000.0f));
    cache.distanceMilliUnits = static_cast<int>(std::lround(distance * 1000.0f));
    cache.materialSignature = materialSignature;
    cache.valid = true;
}

const TextureData* ResolvePreviewTexture(const std::string& path)
{
    if (path.empty())
    {
        return nullptr;
    }

    std::error_code errorCode;
    const std::filesystem::path normalizedPath = NormalizeFilesystemPath(path);
    const bool exists = std::filesystem::exists(normalizedPath, errorCode) && !errorCode;
    auto& cache = GetPreviewTextureCache();
    CachedPreviewTexture& cached = cache[normalizedPath.string()];

    if (!exists)
    {
        cached = CachedPreviewTexture{};
        cached.resolved = true;
        cached.available = false;
        return nullptr;
    }

    const std::filesystem::file_time_type lastWriteTime = std::filesystem::last_write_time(normalizedPath, errorCode);
    const bool reloadRequired =
        !cached.resolved ||
        !cached.available ||
        (!errorCode && cached.lastWriteTime != lastWriteTime);
    if (reloadRequired)
    {
        try
        {
            cached.texture = TextureLoader::LoadRGBA8(normalizedPath.string());
            cached.lastWriteTime = errorCode ? std::filesystem::file_time_type{} : lastWriteTime;
            cached.resolved = true;
            cached.available = cached.texture.IsValid();
        }
        catch (...)
        {
            cached = CachedPreviewTexture{};
            cached.resolved = true;
            cached.available = false;
        }
    }

    return cached.available ? &cached.texture : nullptr;
}

glm::vec3 LinearToSrgb(const glm::vec3& value)
{
    const glm::vec3 clamped = glm::clamp(value, glm::vec3(0.0f), glm::vec3(1.0f));
    return glm::vec3(
        std::pow(clamped.r, 1.0f / 2.2f),
        std::pow(clamped.g, 1.0f / 2.2f),
        std::pow(clamped.b, 1.0f / 2.2f));
}

float WrapRepeat(float value)
{
    value = std::fmod(value, 1.0f);
    if (value < 0.0f)
    {
        value += 1.0f;
    }
    return value;
}

glm::vec4 SampleTextureBilinear(const TextureData& texture, glm::vec2 uv)
{
    if (!texture.IsValid())
    {
        return glm::vec4(1.0f);
    }

    uv.x = WrapRepeat(uv.x);
    uv.y = WrapRepeat(uv.y);

    const float x = uv.x * static_cast<float>(std::max(texture.width - 1, 0));
    const float y = uv.y * static_cast<float>(std::max(texture.height - 1, 0));
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, texture.width - 1);
    const int y1 = std::min(y0 + 1, texture.height - 1);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);

    const auto fetchPixel = [&texture](int pixelX, int pixelY)
    {
        const size_t offset =
            (static_cast<size_t>(pixelY) * static_cast<size_t>(texture.width) + static_cast<size_t>(pixelX)) * 4u;
        return glm::vec4(
            static_cast<float>(texture.pixels[offset + 0]) / 255.0f,
            static_cast<float>(texture.pixels[offset + 1]) / 255.0f,
            static_cast<float>(texture.pixels[offset + 2]) / 255.0f,
            static_cast<float>(texture.pixels[offset + 3]) / 255.0f);
    };

    const glm::vec4 top = glm::mix(fetchPixel(x0, y0), fetchPixel(x1, y0), tx);
    const glm::vec4 bottom = glm::mix(fetchPixel(x0, y1), fetchPixel(x1, y1), tx);
    return glm::mix(top, bottom, ty);
}

glm::vec4 SamplePreviewTexture(
    const std::string& path,
    const glm::vec2& uv,
    const glm::vec4& fallback,
    bool srgb)
{
    const TextureData* texture = ResolvePreviewTexture(path);
    if (texture == nullptr)
    {
        return fallback;
    }

    glm::vec4 sample = SampleTextureBilinear(*texture, uv);
    if (srgb)
    {
        sample.r = std::pow(std::max(sample.r, 0.0f), 2.2f);
        sample.g = std::pow(std::max(sample.g, 0.0f), 2.2f);
        sample.b = std::pow(std::max(sample.b, 0.0f), 2.2f);
    }
    return sample;
}

PreviewSurfaceVertex InterpolatePreviewSurfaceVertex(
    const PreviewSurfaceVertex& a,
    const PreviewSurfaceVertex& b,
    const PreviewSurfaceVertex& c,
    float barycentricA,
    float barycentricB,
    float barycentricC)
{
    PreviewSurfaceVertex result{};
    result.position = a.position * barycentricA + b.position * barycentricB + c.position * barycentricC;
    result.color = a.color * barycentricA + b.color * barycentricB + c.color * barycentricC;
    result.uv = a.uv * barycentricA + b.uv * barycentricB + c.uv * barycentricC;
    result.normal = a.normal * barycentricA + b.normal * barycentricB + c.normal * barycentricC;
    result.tangent = a.tangent * barycentricA + b.tangent * barycentricB + c.tangent * barycentricC;
    return result;
}

glm::vec4 EvaluatePreviewMaterial(
    const ModelImportedMaterialInfo& material,
    const PreviewSurfaceVertex& vertex,
    const glm::vec3& cameraPosition)
{
    const glm::vec4 whiteLinear(1.0f, 1.0f, 1.0f, 1.0f);
    const glm::vec4 flatNormal(0.5f, 0.5f, 1.0f, 1.0f);

    const glm::vec4 primaryBaseColor =
        SamplePreviewTexture(material.baseColorTexturePath, vertex.uv, whiteLinear, true);
    const glm::vec4 secondaryBaseColor = SamplePreviewTexture(
        material.blendGraph.secondaryBaseColorTexturePath.empty()
            ? material.baseColorTexturePath
            : material.blendGraph.secondaryBaseColorTexturePath,
        vertex.uv,
        primaryBaseColor,
        true);
    const glm::vec4 primaryNormal =
        SamplePreviewTexture(material.normalTexturePath, vertex.uv, flatNormal, false);
    const glm::vec4 secondaryNormal = SamplePreviewTexture(
        material.blendGraph.secondaryNormalTexturePath.empty()
            ? material.normalTexturePath
            : material.blendGraph.secondaryNormalTexturePath,
        vertex.uv,
        primaryNormal,
        false);
    const glm::vec4 primaryMetallic =
        SamplePreviewTexture(material.metallicTexturePath, vertex.uv, whiteLinear, false);
    const glm::vec4 secondaryMetallic = SamplePreviewTexture(
        material.blendGraph.secondaryMetallicTexturePath.empty()
            ? material.metallicTexturePath
            : material.blendGraph.secondaryMetallicTexturePath,
        vertex.uv,
        primaryMetallic,
        false);
    const glm::vec4 primaryRoughness =
        SamplePreviewTexture(material.roughnessTexturePath, vertex.uv, whiteLinear, false);
    const glm::vec4 secondaryRoughness = SamplePreviewTexture(
        material.blendGraph.secondaryRoughnessTexturePath.empty()
            ? material.roughnessTexturePath
            : material.blendGraph.secondaryRoughnessTexturePath,
        vertex.uv,
        primaryRoughness,
        false);
    const glm::vec4 primaryOcclusion =
        SamplePreviewTexture(material.occlusionTexturePath, vertex.uv, whiteLinear, false);
    const glm::vec4 secondaryOcclusion = SamplePreviewTexture(
        material.blendGraph.secondaryOcclusionTexturePath.empty()
            ? material.occlusionTexturePath
            : material.blendGraph.secondaryOcclusionTexturePath,
        vertex.uv,
        primaryOcclusion,
        false);
    const glm::vec4 primaryEmissive =
        SamplePreviewTexture(material.emissiveTexturePath, vertex.uv, whiteLinear, true);
    const glm::vec4 secondaryEmissive = SamplePreviewTexture(
        material.blendGraph.secondaryEmissiveTexturePath.empty()
            ? material.emissiveTexturePath
            : material.blendGraph.secondaryEmissiveTexturePath,
        vertex.uv,
        primaryEmissive,
        true);
    const float blendMask = SamplePreviewTexture(
                                material.blendGraph.blendMaskTexturePath,
                                vertex.uv,
                                whiteLinear,
                                false)
                                .r;

    const float blendWeight = glm::clamp(
        (material.blendGraph.enabled ? material.blendGraph.blendFactor : 0.0f) * blendMask,
        0.0f,
        1.0f);

    const glm::vec4 sampledBaseColor = glm::mix(primaryBaseColor, secondaryBaseColor, blendWeight);
    glm::vec4 albedo = sampledBaseColor;
    albedo.r *= vertex.color.r * material.pbr.baseColorFactor[0];
    albedo.g *= vertex.color.g * material.pbr.baseColorFactor[1];
    albedo.b *= vertex.color.b * material.pbr.baseColorFactor[2];
    albedo.a *= material.pbr.baseColorFactor[3] * material.pbr.opacity;

    glm::vec3 geometricNormal = vertex.normal;
    if (glm::length(geometricNormal) < 0.0001f)
    {
        geometricNormal = glm::vec3(0.0f, 0.0f, 1.0f);
    }
    geometricNormal = glm::normalize(geometricNormal);

    glm::vec3 tangent = glm::vec3(vertex.tangent);
    tangent = tangent - geometricNormal * glm::dot(geometricNormal, tangent);
    if (glm::length(tangent) < 0.0001f)
    {
        tangent = glm::normalize(glm::cross(
            std::abs(geometricNormal.z) < 0.999f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f),
            geometricNormal));
    }
    else
    {
        tangent = glm::normalize(tangent);
    }
    const float tangentSign = vertex.tangent.w >= 0.0f ? 1.0f : -1.0f;
    const glm::vec3 bitangent = glm::normalize(glm::cross(geometricNormal, tangent) * tangentSign);

    glm::vec3 sampledNormalPrimary = glm::vec3(primaryNormal) * 2.0f - 1.0f;
    glm::vec3 sampledNormalSecondary = glm::vec3(secondaryNormal) * 2.0f - 1.0f;
    glm::vec3 sampledNormal = glm::normalize(glm::mix(sampledNormalPrimary, sampledNormalSecondary, blendWeight));
    sampledNormal.x *= material.pbr.normalScale;
    sampledNormal.y *= material.pbr.normalScale;
    const glm::mat3 tbn(tangent, bitangent, geometricNormal);
    const glm::vec3 normal = glm::normalize(tbn * sampledNormal);

    const float metallicSample = glm::mix(primaryMetallic.b, secondaryMetallic.b, blendWeight);
    const float roughnessSample = glm::mix(primaryRoughness.g, secondaryRoughness.g, blendWeight);
    const float ambientOcclusionSample = glm::mix(primaryOcclusion.r, secondaryOcclusion.r, blendWeight);
    const glm::vec3 emissiveSample = glm::mix(
        glm::vec3(primaryEmissive),
        glm::vec3(secondaryEmissive),
        blendWeight);

    const float metallic = glm::clamp(material.pbr.metallicFactor * metallicSample, 0.0f, 1.0f);
    const float roughness = glm::clamp(material.pbr.roughnessFactor * roughnessSample, 0.04f, 1.0f);
    const float ambientOcclusion =
        glm::mix(1.0f, ambientOcclusionSample, glm::clamp(material.pbr.occlusionStrength, 0.0f, 1.0f));

    const glm::vec3 viewDirection = glm::normalize(cameraPosition - vertex.position);
    const glm::vec3 lightDirection = glm::normalize(glm::vec3(0.6f, 1.0f, 0.35f));
    const glm::vec3 halfVector = glm::normalize(viewDirection + lightDirection);
    const glm::vec3 radiance = glm::vec3(1.0f, 0.98f, 0.95f) * 2.25f;

    const float nDotL = std::max(glm::dot(normal, lightDirection), 0.0f);
    const float nDotV = std::max(glm::dot(normal, viewDirection), 0.0f);
    const float hDotV = std::max(glm::dot(halfVector, viewDirection), 0.0f);
    const float nDotH = std::max(glm::dot(normal, halfVector), 0.0f);

    const float alpha = roughness * roughness;
    const float alphaSquared = alpha * alpha;
    const float denominator = nDotH * nDotH * (alphaSquared - 1.0f) + 1.0f;
    const float distribution = alphaSquared / std::max(3.14159265359f * denominator * denominator, 0.0001f);

    const auto geometrySchlickGgx = [roughness](float ndotValue)
    {
        const float remappedRoughness = roughness + 1.0f;
        const float k = (remappedRoughness * remappedRoughness) / 8.0f;
        return ndotValue / std::max(ndotValue * (1.0f - k) + k, 0.0001f);
    };
    const float geometry = geometrySchlickGgx(nDotV) * geometrySchlickGgx(nDotL);

    const glm::vec3 baseReflectivity = glm::mix(glm::vec3(0.04f), glm::vec3(albedo), metallic);
    const glm::vec3 fresnel =
        baseReflectivity + (glm::vec3(1.0f) - baseReflectivity) * std::pow(1.0f - hDotV, 5.0f);
    const glm::vec3 specular = (distribution * geometry * fresnel) / std::max(4.0f * nDotV * nDotL, 0.0001f);

    const glm::vec3 diffuseRatio = (glm::vec3(1.0f) - fresnel) * (1.0f - metallic);
    const glm::vec3 diffuse = diffuseRatio * glm::vec3(albedo) / 3.14159265359f;
    const glm::vec3 ambient = glm::vec3(albedo) * 0.2f * ambientOcclusion;
    const glm::vec3 directLighting = (diffuse + specular) * radiance * nDotL;
    const glm::vec3 emissive =
        emissiveSample *
        glm::vec3(material.pbr.emissiveColor[0], material.pbr.emissiveColor[1], material.pbr.emissiveColor[2]) *
        material.pbr.emissiveIntensity;

    glm::vec3 color = ambient + directLighting + emissive;
    color = color / (color + glm::vec3(1.0f));
    color = LinearToSrgb(color);

    return glm::vec4(color, glm::clamp(albedo.a, 0.0f, 1.0f));
}

ImU32 PackPreviewColor(const glm::vec4& color)
{
    const glm::vec4 clamped = glm::clamp(color, glm::vec4(0.0f), glm::vec4(1.0f));
    return IM_COL32(
        static_cast<int>(std::round(clamped.r * 255.0f)),
        static_cast<int>(std::round(clamped.g * 255.0f)),
        static_cast<int>(std::round(clamped.b * 255.0f)),
        static_cast<int>(std::round(clamped.a * 255.0f)));
}

void AddGradientTriangle(
    ImDrawList* drawList,
    const PreviewRasterVertex& a,
    const PreviewRasterVertex& b,
    const PreviewRasterVertex& c)
{
    if (drawList == nullptr || !a.visible || !b.visible || !c.visible)
    {
        return;
    }

    const ImVec2 uv = drawList->_Data->TexUvWhitePixel;
    drawList->PrimReserve(3, 3);
    drawList->PrimWriteIdx(drawList->_VtxCurrentIdx);
    drawList->PrimWriteIdx(static_cast<ImDrawIdx>(drawList->_VtxCurrentIdx + 1));
    drawList->PrimWriteIdx(static_cast<ImDrawIdx>(drawList->_VtxCurrentIdx + 2));
    drawList->PrimWriteVtx(a.screen, uv, a.color);
    drawList->PrimWriteVtx(b.screen, uv, b.color);
    drawList->PrimWriteVtx(c.screen, uv, c.color);
}

size_t DeterminePreviewSubdivisions(float triangleArea, uint32_t totalTriangleCount)
{
    size_t maxSubdivision = 5;
    if (totalTriangleCount > 3500)
    {
        maxSubdivision = 1;
    }
    else if (totalTriangleCount > 1800)
    {
        maxSubdivision = 2;
    }
    else if (totalTriangleCount > 900)
    {
        maxSubdivision = 3;
    }

    if (triangleArea > 9000.0f)
    {
        return maxSubdivision;
    }
    if (triangleArea > 3500.0f)
    {
        return std::min<size_t>(4, maxSubdivision);
    }
    if (triangleArea > 1200.0f)
    {
        return std::min<size_t>(3, maxSubdivision);
    }
    if (triangleArea > 300.0f)
    {
        return std::min<size_t>(2, maxSubdivision);
    }
    return 1;
}

void RebuildMaterialShadedPreviewCache(
    MaterialShadedPreviewCache& cache,
    const LoadedModelData& loadedModel,
    const std::vector<ModelImportedMaterialInfo>& materials,
    int selectedMaterialIndex,
    const ImVec2& canvasMin,
    const ImVec2& canvasSize,
    float yaw,
    float pitch,
    float distance)
{
    const glm::vec3 center = ComputeLoadedModelCenter(loadedModel);
    const float radius = ComputeLoadedModelRadius(loadedModel, center);
    const glm::vec3 viewDirection(
        std::cos(pitch) * std::sin(yaw),
        std::sin(pitch),
        std::cos(pitch) * std::cos(yaw));
    const glm::vec3 eye = center + viewDirection * distance;
    const glm::mat4 view = glm::lookAt(eye, center, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 projection = glm::perspective(
        glm::radians(45.0f),
        std::max(canvasSize.x / std::max(canvasSize.y, 1.0f), 0.1f),
        0.01f,
        std::max(radius * 10.0f + distance, 10.0f));
    const glm::mat4 viewProjection = projection * view;

    uint32_t totalTriangleCount = 0;
    for (const ModelSubmeshData& submesh : loadedModel.submeshes)
    {
        totalTriangleCount += static_cast<uint32_t>(submesh.mesh.indices.size() / 3u);
    }

    cache.triangles.clear();
    cache.triangles.reserve(std::min<size_t>(static_cast<size_t>(totalTriangleCount) * 4u, 24000u));
    cache.cappedTriangles = false;
    constexpr size_t kMaxGeneratedTriangles = kMaxMaterialPreviewTriangles;

    const auto addRasterTriangle = [&](const PreviewSurfaceVertex& a,
                                       const PreviewSurfaceVertex& b,
                                       const PreviewSurfaceVertex& c,
                                       const ModelImportedMaterialInfo& material,
                                       bool dimmed)
    {
        PreviewTriangle triangle{};
        const PreviewSurfaceVertex inputVertices[3] = {a, b, c};
        float accumulatedDepth = 0.0f;

        for (size_t vertexIndex = 0; vertexIndex < 3; ++vertexIndex)
        {
            const PreviewSurfaceVertex& input = inputVertices[vertexIndex];
            const auto projected = ProjectPreviewPoint(input.position, viewProjection, canvasMin, canvasSize);
            if (!projected.has_value())
            {
                return;
            }

            const glm::vec4 viewPosition = view * glm::vec4(input.position, 1.0f);
            glm::vec4 shadedColor = EvaluatePreviewMaterial(material, input, eye);
            if (dimmed)
            {
                shadedColor.r *= 0.72f;
                shadedColor.g *= 0.72f;
                shadedColor.b *= 0.72f;
            }

            triangle.vertices[vertexIndex].screen = *projected;
            triangle.vertices[vertexIndex].depth = -viewPosition.z;
            triangle.vertices[vertexIndex].color = PackPreviewColor(shadedColor);
            triangle.vertices[vertexIndex].visible = true;
            accumulatedDepth += triangle.vertices[vertexIndex].depth;
        }

        triangle.depth = accumulatedDepth / 3.0f;
        cache.triangles.push_back(triangle);
    };

    ModelImportedMaterialInfo fallbackMaterial{};
    for (const ModelSubmeshData& submesh : loadedModel.submeshes)
    {
        if (!submesh.mesh.IsValid())
        {
            continue;
        }

        const ModelImportedMaterialInfo& material =
            submesh.materialIndex < materials.size()
                ? materials[submesh.materialIndex]
                : fallbackMaterial;
        const bool dimmed = selectedMaterialIndex >= 0 &&
                            submesh.materialIndex != static_cast<uint32_t>(selectedMaterialIndex);

        for (size_t index = 0; index + 2 < submesh.mesh.indices.size(); index += 3)
        {
            if (cache.triangles.size() >= kMaxGeneratedTriangles)
            {
                cache.cappedTriangles = true;
                break;
            }

            const uint32_t index0 = submesh.mesh.indices[index];
            const uint32_t index1 = submesh.mesh.indices[index + 1];
            const uint32_t index2 = submesh.mesh.indices[index + 2];
            if (index0 >= submesh.mesh.vertices.size() ||
                index1 >= submesh.mesh.vertices.size() ||
                index2 >= submesh.mesh.vertices.size())
            {
                continue;
            }

            const Vertex& rawVertex0 = submesh.mesh.vertices[index0];
            const Vertex& rawVertex1 = submesh.mesh.vertices[index1];
            const Vertex& rawVertex2 = submesh.mesh.vertices[index2];

            PreviewSurfaceVertex baseVertices[3] = {
                {glm::vec3(rawVertex0.position[0], rawVertex0.position[1], rawVertex0.position[2]),
                 glm::vec3(rawVertex0.color[0], rawVertex0.color[1], rawVertex0.color[2]),
                 glm::vec2(rawVertex0.texCoord[0], rawVertex0.texCoord[1]),
                 glm::vec3(rawVertex0.normal[0], rawVertex0.normal[1], rawVertex0.normal[2]),
                 glm::vec4(rawVertex0.tangent[0], rawVertex0.tangent[1], rawVertex0.tangent[2], rawVertex0.tangent[3])},
                {glm::vec3(rawVertex1.position[0], rawVertex1.position[1], rawVertex1.position[2]),
                 glm::vec3(rawVertex1.color[0], rawVertex1.color[1], rawVertex1.color[2]),
                 glm::vec2(rawVertex1.texCoord[0], rawVertex1.texCoord[1]),
                 glm::vec3(rawVertex1.normal[0], rawVertex1.normal[1], rawVertex1.normal[2]),
                 glm::vec4(rawVertex1.tangent[0], rawVertex1.tangent[1], rawVertex1.tangent[2], rawVertex1.tangent[3])},
                {glm::vec3(rawVertex2.position[0], rawVertex2.position[1], rawVertex2.position[2]),
                 glm::vec3(rawVertex2.color[0], rawVertex2.color[1], rawVertex2.color[2]),
                 glm::vec2(rawVertex2.texCoord[0], rawVertex2.texCoord[1]),
                 glm::vec3(rawVertex2.normal[0], rawVertex2.normal[1], rawVertex2.normal[2]),
                 glm::vec4(rawVertex2.tangent[0], rawVertex2.tangent[1], rawVertex2.tangent[2], rawVertex2.tangent[3])}};

            const auto projected0 = ProjectPreviewPoint(baseVertices[0].position, viewProjection, canvasMin, canvasSize);
            const auto projected1 = ProjectPreviewPoint(baseVertices[1].position, viewProjection, canvasMin, canvasSize);
            const auto projected2 = ProjectPreviewPoint(baseVertices[2].position, viewProjection, canvasMin, canvasSize);
            if (!projected0.has_value() || !projected1.has_value() || !projected2.has_value())
            {
                continue;
            }

            const float triangleArea = std::abs(
                                           ((*projected1).x - (*projected0).x) * ((*projected2).y - (*projected0).y) -
                                           ((*projected2).x - (*projected0).x) * ((*projected1).y - (*projected0).y)) *
                                       0.5f;
            const size_t subdivisions = DeterminePreviewSubdivisions(triangleArea, totalTriangleCount);
            if (subdivisions <= 1)
            {
                addRasterTriangle(baseVertices[0], baseVertices[1], baseVertices[2], material, dimmed);
                continue;
            }

            std::vector<std::vector<PreviewSurfaceVertex>> grid(subdivisions + 1);
            for (size_t row = 0; row <= subdivisions; ++row)
            {
                grid[row].reserve(subdivisions - row + 1);
                for (size_t column = 0; column <= subdivisions - row; ++column)
                {
                    const float barycentricB = static_cast<float>(row) / static_cast<float>(subdivisions);
                    const float barycentricC = static_cast<float>(column) / static_cast<float>(subdivisions);
                    const float barycentricA = 1.0f - barycentricB - barycentricC;
                    grid[row].push_back(InterpolatePreviewSurfaceVertex(
                        baseVertices[0],
                        baseVertices[1],
                        baseVertices[2],
                        barycentricA,
                        barycentricB,
                        barycentricC));
                }
            }

            for (size_t row = 0; row < subdivisions && cache.triangles.size() < kMaxGeneratedTriangles; ++row)
            {
                for (size_t column = 0;
                     column < subdivisions - row && cache.triangles.size() < kMaxGeneratedTriangles;
                     ++column)
                {
                    const PreviewSurfaceVertex& a = grid[row][column];
                    const PreviewSurfaceVertex& b = grid[row + 1][column];
                    const PreviewSurfaceVertex& c = grid[row][column + 1];
                    addRasterTriangle(a, b, c, material, dimmed);

                    if (column + 1 < grid[row + 1].size() && cache.triangles.size() < kMaxGeneratedTriangles)
                    {
                        const PreviewSurfaceVertex& d = grid[row + 1][column + 1];
                        addRasterTriangle(b, d, c, material, dimmed);
                    }
                }
            }
        }

        if (cache.cappedTriangles)
        {
            break;
        }
    }

    std::sort(cache.triangles.begin(), cache.triangles.end(), [](const PreviewTriangle& left, const PreviewTriangle& right)
              {
                  return left.depth > right.depth;
              });
}
}

void DrawMaterialShadedPreview(
    const LoadedModelData& loadedModel,
    const std::vector<ModelImportedMaterialInfo>& materials,
    int selectedMaterialIndex,
    float& yaw,
    float& pitch,
    float& distance,
    bool& autoFramePending,
    float uiScale,
    const char* canvasId,
    const char* overlayLabel)
{
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 canvasSize(
        std::max(available.x, 260.0f * uiScale),
        std::max(260.0f * uiScale, std::min(available.y, 360.0f * uiScale)));

    ImGui::InvisibleButton(canvasId, canvasSize, ImGuiButtonFlags_MouseButtonLeft);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
    const ImVec2 canvasMin = ImGui::GetItemRectMin();
    const ImVec2 canvasMax = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilledMultiColor(
        canvasMin,
        canvasMax,
        IM_COL32(12, 16, 24, 255),
        IM_COL32(18, 24, 36, 255),
        IM_COL32(28, 34, 48, 255),
        IM_COL32(18, 20, 30, 255));
    drawList->AddRect(canvasMin, canvasMax, IM_COL32(92, 108, 132, 255), 10.0f * uiScale, 0, 1.25f);

    const bool previewHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (previewHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        yaw += ImGui::GetIO().MouseDelta.x * 0.01f;
        pitch = std::clamp(pitch + ImGui::GetIO().MouseDelta.y * 0.01f, -1.35f, 1.35f);
    }
    if (previewHovered && std::abs(ImGui::GetIO().MouseWheel) > 0.0f)
    {
        distance = std::max(0.2f, distance * (1.0f - ImGui::GetIO().MouseWheel * 0.12f));
    }

    if (!loadedModel.IsValid())
    {
        drawList->AddText(
            ImVec2(canvasMin.x + 14.0f * uiScale, canvasMin.y + 14.0f * uiScale),
            IM_COL32(218, 224, 236, 255),
            "Unable to preview this model.");
        return;
    }

    const glm::vec3 center = ComputeLoadedModelCenter(loadedModel);
    const float radius = ComputeLoadedModelRadius(loadedModel, center);
    if (autoFramePending)
    {
        distance = std::max(radius * 2.75f, 0.8f);
        autoFramePending = false;
    }

    const size_t materialSignature = ComputePreviewMaterialSignature(materials);
    auto& previewCache = GetMaterialShadedPreviewCaches()[canvasId];
    if (!IsMatchingMaterialShadedPreviewCache(
            previewCache,
            canvasMin,
            canvasSize,
            selectedMaterialIndex,
            yaw,
            pitch,
            distance,
            materialSignature))
    {
        RebuildMaterialShadedPreviewCache(
            previewCache,
            loadedModel,
            materials,
            selectedMaterialIndex,
            canvasMin,
            canvasSize,
            yaw,
            pitch,
            distance);
        StoreMaterialShadedPreviewCacheKey(
            previewCache,
            canvasMin,
            canvasSize,
            selectedMaterialIndex,
            yaw,
            pitch,
            distance,
            materialSignature);
    }

    for (const PreviewTriangle& triangle : previewCache.triangles)
    {
        AddGradientTriangle(drawList, triangle.vertices[0], triangle.vertices[1], triangle.vertices[2]);
    }

    drawList->AddText(
        ImVec2(canvasMin.x + 12.0f * uiScale, canvasMin.y + 12.0f * uiScale),
        IM_COL32(218, 224, 236, 255),
        overlayLabel);
    drawList->AddText(
        ImVec2(canvasMin.x + 12.0f * uiScale, canvasMax.y - 26.0f * uiScale),
        IM_COL32(176, 188, 206, 255),
        previewCache.cappedTriangles ? "Approximate preview capped for responsiveness" : "Drag to orbit, wheel to zoom");
}
