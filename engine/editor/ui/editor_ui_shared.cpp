#include "editor_ui_internal.h"

#include <engine/asset/material_graph_runtime.h>
#include <engine/asset/model_loader.h>
#include <engine/asset/texture_loader.h>

#include <engine/logic/editor_world.h>
#include <engine/platform/file_dialog/file_dialog.h>
#include <engine/core/log/log.h>
#include <engine/platform/ui/ui_scale.h>
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

namespace me
{

namespace
{
void DrawTexturePathRow(const char* label, const std::string& path)
{
    if (path.empty())
    {
        ImGui::TextDisabled("%s: <none>", label);
        return;
    }

    std::error_code errorCode;
    const bool exists = std::filesystem::exists(path, errorCode);
    const ImVec4 statusColor = exists ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f) : ImVec4(0.95f, 0.4f, 0.4f, 1.0f);
    ImGui::TextWrapped("%s: %s", label, path.c_str());
    ImGui::SameLine();
    ImGui::TextColored(statusColor, "[%s]", exists ? "resolved" : "missing");
}

size_t CountMaterialGraphSecondaryTextures(const MaterialTextureBlendGraph& blendGraph);

struct MaterialTexturePathRow
{
    const char* label = "";
    const std::string ModelImportedMaterialInfo::* path = nullptr;
};

struct BlendGraphTexturePathRow
{
    const char* label = "";
    const std::string MaterialTextureBlendGraph::* path = nullptr;
};

constexpr std::array<MaterialTexturePathRow, 6> kPrimaryMaterialTextureRows = {{{"Base Color", &ModelImportedMaterialInfo::baseColorTexturePath},
                                                                                {"Normal", &ModelImportedMaterialInfo::normalTexturePath},
                                                                                {"Metallic", &ModelImportedMaterialInfo::metallicTexturePath},
                                                                                {"Roughness", &ModelImportedMaterialInfo::roughnessTexturePath},
                                                                                {"Occlusion", &ModelImportedMaterialInfo::occlusionTexturePath},
                                                                                {"Emissive", &ModelImportedMaterialInfo::emissiveTexturePath}}};

constexpr std::array<BlendGraphTexturePathRow, 7> kSecondaryMaterialTextureRows = {{{"Blend Mask", &MaterialTextureBlendGraph::blendMaskTexturePath},
                                                                                    {"Layer B Base", &MaterialTextureBlendGraph::secondaryBaseColorTexturePath},
                                                                                    {"Layer B Normal", &MaterialTextureBlendGraph::secondaryNormalTexturePath},
                                                                                    {"Layer B Metallic", &MaterialTextureBlendGraph::secondaryMetallicTexturePath},
                                                                                    {"Layer B Roughness", &MaterialTextureBlendGraph::secondaryRoughnessTexturePath},
                                                                                    {"Layer B Occlusion", &MaterialTextureBlendGraph::secondaryOcclusionTexturePath},
                                                                                    {"Layer B Emissive", &MaterialTextureBlendGraph::secondaryEmissiveTexturePath}}};

template <typename TObject, typename TEntry, size_t TSize>
void DrawTexturePathRows(const TObject& object, const std::array<TEntry, TSize>& rows)
{
    for (const TEntry& row : rows)
    {
        DrawTexturePathRow(row.label, object.*(row.path));
    }
}
}

bool HasSecondaryMaterialLayer(const MaterialTextureBlendGraph& blendGraph)
{
    return blendGraph.enabled || CountMaterialGraphSecondaryTextures(blendGraph) > 0;
}

void DrawPrimaryMaterialTextureRows(const ModelImportedMaterialInfo& material)
{
    DrawTexturePathRows(material, kPrimaryMaterialTextureRows);
}

void DrawSecondaryMaterialTextureRows(const MaterialTextureBlendGraph& blendGraph)
{
    DrawTexturePathRows(blendGraph, kSecondaryMaterialTextureRows);
}

namespace
{
size_t CountMaterialGraphSecondaryTextures(const MaterialTextureBlendGraph& blendGraph)
{
    const std::array<const std::string*, 7> paths = {
        &blendGraph.secondaryBaseColorTexturePath,
        &blendGraph.secondaryNormalTexturePath,
        &blendGraph.secondaryMetallicTexturePath,
        &blendGraph.secondaryRoughnessTexturePath,
        &blendGraph.secondaryOcclusionTexturePath,
        &blendGraph.secondaryEmissiveTexturePath,
        &blendGraph.blendMaskTexturePath};
    return static_cast<size_t>(std::count_if(paths.begin(), paths.end(), [](const std::string* value)
                                             {
                                                 return value != nullptr && !value->empty();
                                             }));
}
}

bool IsSupportedModelAssetPath(const std::filesystem::path& path)
{
    return ModelLoader::IsSupportedModelPath(path);
}

std::filesystem::path NormalizeFilesystemPath(const std::filesystem::path& path)
{
    std::error_code errorCode;
    const std::filesystem::path absolutePath = std::filesystem::absolute(path, errorCode);
    return errorCode ? path.lexically_normal() : absolutePath.lexically_normal();
}

const char* GetLightTypeLabel(LightType type)
{
    switch (type)
    {
    case LightType::Directional:
        return "Directional";
    case LightType::Point:
        return "Point";
    case LightType::Spot:
        return "Spot";
    case LightType::Area:
        return "Area";
    case LightType::Ambient:
        return "Ambient";
    default:
        return "Unknown";
    }
}

ImU32 GetLightTypeColor(LightType type)
{
    switch (type)
    {
    case LightType::Directional:
        return IM_COL32(255, 240, 128, 255);
    case LightType::Point:
        return IM_COL32(255, 196, 64, 255);
    case LightType::Spot:
        return IM_COL32(128, 220, 255, 255);
    case LightType::Area:
        return IM_COL32(180, 255, 160, 255);
    case LightType::Ambient:
        return IM_COL32(200, 180, 255, 255);
    default:
        return IM_COL32(220, 220, 220, 255);
    }
}
}
