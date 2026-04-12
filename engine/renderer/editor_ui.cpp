#include "editor_ui.h"
#include "material_graph_runtime.h"
#include "model_loader.h"
#include "texture_loader.h"

#include <editor_world.h>
#include <file_dialog/file_dialog.h>
#include <log/log.h>
#include <ui/ui_scale.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <yaml-cpp/yaml.h>
#include <glm/common.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
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
constexpr ImU32 kSelectionOutlineColor = IM_COL32(255, 196, 64, 255);
constexpr ImU32 kViewportDropOverlayColor = IM_COL32(88, 136, 92, 96);
constexpr ImU32 kViewportDropOutlineColor = IM_COL32(164, 220, 168, 255);
constexpr ImU32 kAssetPaneBackgroundColor = IM_COL32(22, 26, 33, 255);
constexpr ImU32 kAssetTileBackgroundColor = IM_COL32(34, 40, 49, 255);
constexpr ImU32 kAssetTileHoverColor = IM_COL32(47, 56, 69, 255);
constexpr ImU32 kAssetTileSelectedColor = IM_COL32(70, 85, 104, 255);
constexpr ImU32 kAssetTileBorderColor = IM_COL32(82, 92, 108, 255);
constexpr ImU32 kAssetTileHoverBorderColor = IM_COL32(124, 141, 166, 255);
constexpr float kSelectionCenterHitRadiusPixels = 20.0f;
constexpr float kSelectionBoundsHitPaddingPixels = 6.0f;
constexpr float kSelectionOutlineThickness = 2.0f;
constexpr float kMaterialGraphMinZoom = 0.55f;
constexpr float kMaterialGraphMaxZoom = 1.8f;
constexpr float kMaterialGraphZoomStep = 1.12f;
constexpr float kMaterialGraphResizeBorderPixels = 10.0f;
constexpr size_t kMaxMaterialPreviewTriangles = 12000;
constexpr size_t kMaxUvPreviewTriangles = 2400;
constexpr ImGuiDockNodeFlags kEditorDockspaceFlags = ImGuiDockNodeFlags_PassthruCentralNode;
constexpr const char* kAssetModelDragDropPayloadId = "MiniEngineAssetModelPath";

struct ProjectedEntityCenter
{
    entt::entity entity = entt::null;
    ImVec2 center{ 0.0f, 0.0f };
    ImVec2 min{ 0.0f, 0.0f };
    ImVec2 max{ 0.0f, 0.0f };
    float depth = std::numeric_limits<float>::max();
};

struct ViewportOverlayRect
{
    ImVec2 origin{ 0.0f, 0.0f };
    ImVec2 size{ 0.0f, 0.0f };
    ImDrawList* drawList = nullptr;
    bool hovered = false;
    bool focused = false;
};

struct ImportedModelFingerprint
{
    uintmax_t fileSize = 0;
    std::string contentHash;
};

struct DuplicateModelImportInfo
{
    std::string sourcePath;
    std::string existingAssetPath;
};

struct ThemeColorEntry
{
    const char* label = "";
    ImGuiCol colorId = ImGuiCol_Text;
};

bool DrawThemeColorSection(const char* title, const ThemeColorEntry* entries, size_t entryCount)
{
    if (!ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen))
    {
        return false;
    }

    bool changed = false;
    ImGuiStyle& style = ImGui::GetStyle();
    for (size_t index = 0; index < entryCount; ++index)
    {
        changed |= ImGui::ColorEdit4(entries[index].label, &style.Colors[entries[index].colorId].x);
    }
    return changed;
}

constexpr std::array<ThemeColorEntry, 8> kThemeSurfaceEntries = { {
    { "Window Background", ImGuiCol_WindowBg },
    { "Child Background", ImGuiCol_ChildBg },
    { "Popup Background", ImGuiCol_PopupBg },
    { "Frame Background", ImGuiCol_FrameBg },
    { "Frame Hovered", ImGuiCol_FrameBgHovered },
    { "Frame Active", ImGuiCol_FrameBgActive },
    { "Menu Bar", ImGuiCol_MenuBarBg },
    { "Scrollbar Background", ImGuiCol_ScrollbarBg }
} };

constexpr std::array<ThemeColorEntry, 10> kThemeControlEntries = { {
    { "Button", ImGuiCol_Button },
    { "Button Hovered", ImGuiCol_ButtonHovered },
    { "Button Active", ImGuiCol_ButtonActive },
    { "Header", ImGuiCol_Header },
    { "Header Hovered", ImGuiCol_HeaderHovered },
    { "Header Active", ImGuiCol_HeaderActive },
    { "Check Mark", ImGuiCol_CheckMark },
    { "Slider Grab", ImGuiCol_SliderGrab },
    { "Slider Grab Active", ImGuiCol_SliderGrabActive },
    { "Separator", ImGuiCol_Separator }
} };

constexpr std::array<ThemeColorEntry, 7> kThemeChromeEntries = { {
    { "Title Background", ImGuiCol_TitleBg },
    { "Title Active", ImGuiCol_TitleBgActive },
    { "Title Collapsed", ImGuiCol_TitleBgCollapsed },
    { "Border", ImGuiCol_Border },
    { "Resize Grip", ImGuiCol_ResizeGrip },
    { "Resize Grip Hovered", ImGuiCol_ResizeGripHovered },
    { "Resize Grip Active", ImGuiCol_ResizeGripActive }
} };

constexpr std::array<ThemeColorEntry, 7> kThemeTabDockEntries = { {
    { "Tab", ImGuiCol_Tab },
    { "Tab Hovered", ImGuiCol_TabHovered },
    { "Tab Active", ImGuiCol_TabActive },
    { "Tab Unfocused", ImGuiCol_TabUnfocused },
    { "Tab Unfocused Active", ImGuiCol_TabUnfocusedActive },
    { "Docking Preview", ImGuiCol_DockingPreview },
    { "Docking Empty Background", ImGuiCol_DockingEmptyBg }
} };

constexpr std::array<ThemeColorEntry, 9> kThemeStateEntries = { {
    { "Text", ImGuiCol_Text },
    { "Text Disabled", ImGuiCol_TextDisabled },
    { "Scrollbar Grab", ImGuiCol_ScrollbarGrab },
    { "Scrollbar Grab Hovered", ImGuiCol_ScrollbarGrabHovered },
    { "Scrollbar Grab Active", ImGuiCol_ScrollbarGrabActive },
    { "Table Header", ImGuiCol_TableHeaderBg },
    { "Table Border Strong", ImGuiCol_TableBorderStrong },
    { "Table Border Light", ImGuiCol_TableBorderLight },
    { "Table Row Alt", ImGuiCol_TableRowBgAlt }
} };

constexpr std::array<ThemeColorEntry, 5> kThemeFeedbackEntries = { {
    { "Text Selection", ImGuiCol_TextSelectedBg },
    { "Drag Drop Target", ImGuiCol_DragDropTarget },
    { "Navigation Cursor", ImGuiCol_NavCursor },
    { "Navigation Highlight", ImGuiCol_NavWindowingHighlight },
    { "Separator Hovered", ImGuiCol_SeparatorHovered }
} };

constexpr std::array<ThemeColorEntry, 1> kThemeFeedbackActiveEntries = { {
    { "Separator Active", ImGuiCol_SeparatorActive }
} };

ViewportOverlayRect BuildViewportOverlayRect(ImTextureID viewportTextureId, bool flipViewportImageY)
{
    ViewportOverlayRect rect{};
    rect.drawList = ImGui::GetWindowDrawList();

    ImVec2 available = ImGui::GetContentRegionAvail();
    available.x = std::max(available.x, 1.0f);
    available.y = std::max(available.y, 1.0f);

    if (viewportTextureId)
    {
        const ImVec2 uv0 = flipViewportImageY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
        const ImVec2 uv1 = flipViewportImageY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
        ImGui::Image(viewportTextureId, available, uv0, uv1);
    }
    else
    {
        ImGui::Dummy(available);
    }

    rect.origin = ImGui::GetItemRectMin();
    rect.size = ImGui::GetItemRectSize();
    rect.hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    rect.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    return rect;
}

RenderExtent BuildViewportExtent(const ViewportOverlayRect& rect)
{
    return RenderExtent{
        std::max(static_cast<uint32_t>(std::lround(rect.size.x)), 1u),
        std::max(static_cast<uint32_t>(std::lround(rect.size.y)), 1u)
    };
}

void DrawViewportOverlay(const ViewportOverlayRect& rect, ImTextureID viewportTextureId)
{
    if (rect.drawList == nullptr)
    {
        return;
    }

    const ImVec2 max(rect.origin.x + rect.size.x, rect.origin.y + rect.size.y);
    if (!viewportTextureId)
    {
        rect.drawList->AddRectFilled(rect.origin, max, IM_COL32(18, 22, 30, 255));
    }

    rect.drawList->AddRect(rect.origin, max, IM_COL32(255, 255, 255, 48), 0.0f, 0, 1.0f);
}

void EnsureDefaultDockLayout(ImGuiID dockspaceId, const ImVec2& dockspaceSize)
{
    ImGuiDockNode* dockNode = ImGui::DockBuilderGetNode(dockspaceId);
    if (dockNode != nullptr &&
        (dockNode->ChildNodes[0] != nullptr || dockNode->ChildNodes[1] != nullptr || dockNode->Windows.Size > 0))
    {
        return;
    }

    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    if (mainViewport == nullptr)
    {
        return;
    }

    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, kEditorDockspaceFlags | ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, dockspaceSize);

    ImGuiID leftNode = 0;
    ImGuiID rightNode = 0;
    ImGuiID centerNode = dockspaceId;
    ImGui::DockBuilderSplitNode(centerNode, ImGuiDir_Left, 0.28f, &leftNode, &centerNode);
    ImGui::DockBuilderSplitNode(centerNode, ImGuiDir_Right, 0.24f, &rightNode, &centerNode);

    ImGuiID lowerRightNode = 0;
    ImGuiID upperRightNode = rightNode;
    ImGui::DockBuilderSplitNode(upperRightNode, ImGuiDir_Down, 0.42f, &lowerRightNode, &upperRightNode);

    ImGui::DockBuilderDockWindow("Scene", leftNode);
    ImGui::DockBuilderDockWindow("Asset Manager", upperRightNode);
    ImGui::DockBuilderDockWindow("Camera", lowerRightNode);
    ImGui::DockBuilderDockWindow("Viewport", centerNode);
    ImGui::DockBuilderFinish(dockspaceId);
}

std::array<glm::vec3, 8> BuildBoundsCorners(const glm::vec3& minBounds, const glm::vec3& maxBounds)
{
    return {
        glm::vec3(minBounds.x, minBounds.y, minBounds.z),
        glm::vec3(maxBounds.x, minBounds.y, minBounds.z),
        glm::vec3(minBounds.x, maxBounds.y, minBounds.z),
        glm::vec3(maxBounds.x, maxBounds.y, minBounds.z),
        glm::vec3(minBounds.x, minBounds.y, maxBounds.z),
        glm::vec3(maxBounds.x, minBounds.y, maxBounds.z),
        glm::vec3(minBounds.x, maxBounds.y, maxBounds.z),
        glm::vec3(maxBounds.x, maxBounds.y, maxBounds.z)
    };
}

constexpr std::array<std::pair<size_t, size_t>, 12> kBoundsEdges = { {
    { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 },
    { 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 },
    { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
} };

std::string ToLowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool MaterialHasAnyTexture(const ModelImportedMaterialInfo& material)
{
    return
        !material.baseColorTexturePath.empty() ||
        !material.normalTexturePath.empty() ||
        !material.metallicTexturePath.empty() ||
        !material.roughnessTexturePath.empty() ||
        !material.occlusionTexturePath.empty() ||
        !material.emissiveTexturePath.empty();
}

std::string ReadDragDropPayloadString(const ImGuiPayload& payload)
{
    if (payload.Data == nullptr || payload.DataSize <= 0)
    {
        return {};
    }

    const size_t stringLength =
        payload.DataSize > 0
        ? static_cast<size_t>(payload.DataSize - 1)
        : static_cast<size_t>(payload.DataSize);
    return std::string(static_cast<const char*>(payload.Data), stringLength);
}

uint32_t CountUvReadySubmeshes(const ModelComponent& model)
{
    return static_cast<uint32_t>(std::count_if(
        model.importedSubmeshes.begin(),
        model.importedSubmeshes.end(),
        [](const ModelImportedSubmeshInfo& submesh)
        {
            return submesh.hasTexCoords;
        }
    ));
}

uint32_t CountTexturedMaterials(const ModelComponent& model)
{
    return static_cast<uint32_t>(std::count_if(
        model.importedMaterials.begin(),
        model.importedMaterials.end(),
        [](const ModelImportedMaterialInfo& material)
        {
            return MaterialHasAnyTexture(material);
        }
    ));
}

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

constexpr std::array<MaterialTexturePathRow, 6> kPrimaryMaterialTextureRows = { {
    { "Base Color", &ModelImportedMaterialInfo::baseColorTexturePath },
    { "Normal", &ModelImportedMaterialInfo::normalTexturePath },
    { "Metallic", &ModelImportedMaterialInfo::metallicTexturePath },
    { "Roughness", &ModelImportedMaterialInfo::roughnessTexturePath },
    { "Occlusion", &ModelImportedMaterialInfo::occlusionTexturePath },
    { "Emissive", &ModelImportedMaterialInfo::emissiveTexturePath }
} };

constexpr std::array<BlendGraphTexturePathRow, 7> kSecondaryMaterialTextureRows = { {
    { "Blend Mask", &MaterialTextureBlendGraph::blendMaskTexturePath },
    { "Layer B Base", &MaterialTextureBlendGraph::secondaryBaseColorTexturePath },
    { "Layer B Normal", &MaterialTextureBlendGraph::secondaryNormalTexturePath },
    { "Layer B Metallic", &MaterialTextureBlendGraph::secondaryMetallicTexturePath },
    { "Layer B Roughness", &MaterialTextureBlendGraph::secondaryRoughnessTexturePath },
    { "Layer B Occlusion", &MaterialTextureBlendGraph::secondaryOcclusionTexturePath },
    { "Layer B Emissive", &MaterialTextureBlendGraph::secondaryEmissiveTexturePath }
} };

template <typename TObject, typename TEntry, size_t TSize>
void DrawTexturePathRows(const TObject& object, const std::array<TEntry, TSize>& rows)
{
    for (const TEntry& row : rows)
    {
        DrawTexturePathRow(row.label, object.*(row.path));
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

ModelImportedMaterialInfo BuildImportedMaterialInfo(const ModelMaterialData& material)
{
    ModelImportedMaterialInfo importedMaterial{
        material.name,
        material.baseColorTexturePath,
        material.normalTexturePath,
        material.metallicTexturePath,
        material.roughnessTexturePath,
        material.occlusionTexturePath,
        material.emissiveTexturePath,
        material.pbr,
        material.blendGraph,
        material.shaderGraph
    };
    EnsureMaterialShaderGraph(importedMaterial.name, std::nullopt, importedMaterial);
    CompileMaterialShaderGraph(importedMaterial);
    return importedMaterial;
}

std::string BuildMaterialSlotLabel(const ModelImportedMaterialInfo& material, size_t materialIndex)
{
    return material.name.empty()
        ? ("Material " + std::to_string(materialIndex))
        : material.name;
}

ModelImportedMaterialInfo ReadImportedMaterialInfoFromYamlNode(
    const YAML::Node& materialNode,
    const std::string& fallbackName
)
{
    if (!materialNode || !materialNode.IsMap())
    {
        throw std::runtime_error("The selected material asset does not contain a valid 'material' map.");
    }

    ModelImportedMaterialInfo material{};
    material.name = materialNode["name"].as<std::string>(fallbackName);
    material.baseColorTexturePath = materialNode["base_color_texture_path"].as<std::string>(material.baseColorTexturePath);
    material.normalTexturePath = materialNode["normal_texture_path"].as<std::string>(material.normalTexturePath);
    material.metallicTexturePath = materialNode["metallic_texture_path"].as<std::string>(material.metallicTexturePath);
    material.roughnessTexturePath = materialNode["roughness_texture_path"].as<std::string>(material.roughnessTexturePath);
    material.occlusionTexturePath = materialNode["occlusion_texture_path"].as<std::string>(material.occlusionTexturePath);
    material.emissiveTexturePath = materialNode["emissive_texture_path"].as<std::string>(material.emissiveTexturePath);
    if (const YAML::Node pbrNode = materialNode["pbr"]; pbrNode && pbrNode.IsMap())
    {
        if (pbrNode["base_color_factor"] && pbrNode["base_color_factor"].IsSequence() && pbrNode["base_color_factor"].size() == 4)
        {
            for (size_t index = 0; index < 4; ++index)
            {
                material.pbr.baseColorFactor[index] = pbrNode["base_color_factor"][index].as<float>(material.pbr.baseColorFactor[index]);
            }
        }
        if (pbrNode["emissive_color"] && pbrNode["emissive_color"].IsSequence() && pbrNode["emissive_color"].size() == 3)
        {
            for (size_t index = 0; index < 3; ++index)
            {
                material.pbr.emissiveColor[index] = pbrNode["emissive_color"][index].as<float>(material.pbr.emissiveColor[index]);
            }
        }
        material.pbr.metallicFactor = pbrNode["metallic_factor"].as<float>(material.pbr.metallicFactor);
        material.pbr.roughnessFactor = pbrNode["roughness_factor"].as<float>(material.pbr.roughnessFactor);
        material.pbr.normalScale = pbrNode["normal_scale"].as<float>(material.pbr.normalScale);
        material.pbr.occlusionStrength = pbrNode["occlusion_strength"].as<float>(material.pbr.occlusionStrength);
        material.pbr.emissiveIntensity = pbrNode["emissive_intensity"].as<float>(material.pbr.emissiveIntensity);
        material.pbr.opacity = pbrNode["opacity"].as<float>(material.pbr.opacity);
    }
    if (const YAML::Node graphNode = materialNode["texture_graph"]; graphNode && graphNode.IsMap())
    {
        material.blendGraph.enabled = graphNode["enabled"].as<bool>(material.blendGraph.enabled);
        material.blendGraph.blendFactor = graphNode["blend_factor"].as<float>(material.blendGraph.blendFactor);
        material.blendGraph.blendMaskTexturePath = graphNode["blend_mask_texture_path"].as<std::string>(material.blendGraph.blendMaskTexturePath);
        material.blendGraph.secondaryBaseColorTexturePath = graphNode["secondary_base_color_texture_path"].as<std::string>(material.blendGraph.secondaryBaseColorTexturePath);
        material.blendGraph.secondaryNormalTexturePath = graphNode["secondary_normal_texture_path"].as<std::string>(material.blendGraph.secondaryNormalTexturePath);
        material.blendGraph.secondaryMetallicTexturePath = graphNode["secondary_metallic_texture_path"].as<std::string>(material.blendGraph.secondaryMetallicTexturePath);
        material.blendGraph.secondaryRoughnessTexturePath = graphNode["secondary_roughness_texture_path"].as<std::string>(material.blendGraph.secondaryRoughnessTexturePath);
        material.blendGraph.secondaryOcclusionTexturePath = graphNode["secondary_occlusion_texture_path"].as<std::string>(material.blendGraph.secondaryOcclusionTexturePath);
        material.blendGraph.secondaryEmissiveTexturePath = graphNode["secondary_emissive_texture_path"].as<std::string>(material.blendGraph.secondaryEmissiveTexturePath);
    }
    DeserializeMaterialShaderGraph(materialNode["shader_graph"], material.name, std::nullopt, material);
    CompileMaterialShaderGraph(material);

    material.blendGraph.blendFactor = std::clamp(material.blendGraph.blendFactor, 0.0f, 1.0f);
    material.pbr.baseColorFactor[0] = std::clamp(material.pbr.baseColorFactor[0], 0.0f, 4.0f);
    material.pbr.baseColorFactor[1] = std::clamp(material.pbr.baseColorFactor[1], 0.0f, 4.0f);
    material.pbr.baseColorFactor[2] = std::clamp(material.pbr.baseColorFactor[2], 0.0f, 4.0f);
    material.pbr.baseColorFactor[3] = std::clamp(material.pbr.baseColorFactor[3], 0.0f, 1.0f);
    material.pbr.metallicFactor = std::clamp(material.pbr.metallicFactor, 0.0f, 1.0f);
    material.pbr.roughnessFactor = std::clamp(material.pbr.roughnessFactor, 0.0f, 1.0f);
    material.pbr.normalScale = std::clamp(material.pbr.normalScale, 0.0f, 4.0f);
    material.pbr.occlusionStrength = std::clamp(material.pbr.occlusionStrength, 0.0f, 1.0f);
    material.pbr.emissiveIntensity = std::max(material.pbr.emissiveIntensity, 0.0f);
    material.pbr.opacity = std::clamp(material.pbr.opacity, 0.0f, 1.0f);
    if (material.name.empty())
    {
        material.name = fallbackName;
    }
    return material;
}

std::vector<std::string> LoadImportedModelMaterialAssetPaths(
    const std::filesystem::path& modelPath,
    size_t materialCount
)
{
    std::vector<std::string> materialAssetPaths(materialCount);
    const std::filesystem::path manifestPath = std::filesystem::path(modelPath.string() + ".miniengine_asset.yaml");
    if (!std::filesystem::exists(manifestPath))
    {
        return materialAssetPaths;
    }

    try
    {
        const YAML::Node root = YAML::LoadFile(manifestPath.string());
        const YAML::Node materialsNode = root["materials"];
        if (!materialsNode || !materialsNode.IsSequence())
        {
            return materialAssetPaths;
        }

        const size_t count = std::min(materialCount, materialsNode.size());
        for (size_t materialIndex = 0; materialIndex < count; ++materialIndex)
        {
            const YAML::Node materialNode = materialsNode[materialIndex];
            if (!materialNode || !materialNode.IsMap())
            {
                continue;
            }

            if (const YAML::Node value = materialNode["material_asset_path"]; value)
            {
                materialAssetPaths[materialIndex] = value.as<std::string>();
            }
        }
    }
    catch (...)
    {
        // Ignore invalid sidecar metadata and keep the UI functional.
    }

    return materialAssetPaths;
}

std::string ImportTextureIntoModelMaterialDirectory(
    const std::string& modelPath,
    uint32_t materialIndex,
    const char* slotName,
    const std::string& sourceTexturePath
);

void DrawImportedModelInspector(const ModelComponent& model)
{
    if (model.importedSubmeshes.empty() && model.importedMaterials.empty())
    {
        return;
    }

    ImGui::Separator();
    ImGui::Text("Importer: %s", ModelLoader::GetImporterName());
    ImGui::Text(
        "UV Submeshes: %u / %u",
        CountUvReadySubmeshes(model),
        static_cast<unsigned int>(model.importedSubmeshes.size())
    );
    ImGui::Text(
        "Textured Materials: %u / %u",
        CountTexturedMaterials(model),
        static_cast<unsigned int>(model.importedMaterials.size())
    );
    ImGui::TextWrapped("Texture bindings are applied only when the imported submesh carries valid UVs.");

    if (ImGui::TreeNode("Imported Submeshes"))
    {
        for (size_t submeshIndex = 0; submeshIndex < model.importedSubmeshes.size(); ++submeshIndex)
        {
            const ModelImportedSubmeshInfo& submesh = model.importedSubmeshes[submeshIndex];
            const std::string treeLabel =
                submesh.name.empty()
                ? ("Submesh " + std::to_string(submeshIndex))
                : (submesh.name + "##submesh_" + std::to_string(submeshIndex));
            if (!ImGui::TreeNode(treeLabel.c_str()))
            {
                continue;
            }

            ImGui::Text("Vertices: %u", submesh.vertexCount);
            ImGui::Text("Indices: %u", submesh.indexCount);
            ImGui::Text("Triangles: %u", submesh.indexCount / 3u);
            ImGui::Text("Material Slot: %u", submesh.materialIndex);
            ImGui::Text("Has UV: %s", submesh.hasTexCoords ? "Yes" : "No");
            ImGui::Text("Has Normal: %s", submesh.hasNormals ? "Yes" : "No");
            ImGui::Text("Has Tangent: %s", submesh.hasTangents ? "Yes" : "No");
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Imported Materials"))
    {
        for (size_t materialIndex = 0; materialIndex < model.importedMaterials.size(); ++materialIndex)
        {
            const ModelImportedMaterialInfo& material = model.importedMaterials[materialIndex];
            const MaterialTextureBlendGraph& blendGraph = material.blendGraph;
            const bool hasProgrammableGraph = HasSecondaryMaterialLayer(blendGraph);
            const std::string materialName = material.name.empty()
                ? ("Material " + std::to_string(materialIndex))
                : material.name;
            const std::string treeLabel =
                (hasProgrammableGraph ? "[PBR Graph] " : "") + materialName + "##material_" + std::to_string(materialIndex);
            if (!ImGui::TreeNode(treeLabel.c_str()))
            {
                continue;
            }

            DrawPrimaryMaterialTextureRows(material);
            if (hasProgrammableGraph)
            {
                ImGui::Separator();
                ImGui::Text("Programmable Blend: %s", blendGraph.enabled ? "Enabled" : "Prepared");
                ImGui::Text("Blend Factor: %.2f", blendGraph.blendFactor);
                DrawSecondaryMaterialTextureRows(blendGraph);
            }
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }
}

void DrawSelectedModelUvTextureControls(
    const ModelComponent* model,
    EditorUiFrameResult& result,
    bool compactLabels = false
)
{
    const bool hasSelection = model != nullptr;
    const bool hasImportedModel = hasSelection && !model->sourcePath.empty();
    const bool hasUvSubmeshes = hasSelection && CountUvReadySubmeshes(*model) > 0;
    const bool hasOverride = hasSelection && !model->baseColorTextureOverridePath.empty();

    const char* applyLabel = compactLabels ? "Apply UV Texture##compact" : "Apply UV Texture";
    const char* clearLabel = compactLabels ? "Clear UV Texture##compact" : "Clear UV Texture";

    ImGui::BeginDisabled(!hasImportedModel || !hasUvSubmeshes);
    if (ImGui::Button(applyLabel))
    {
        result.actions.selectedBaseColorTexturePath = OpenTextureFileDialog();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();

    ImGui::BeginDisabled(!hasOverride);
    if (ImGui::Button(clearLabel))
    {
        result.actions.clearSelectedBaseColorTexture = true;
    }
    ImGui::EndDisabled();

    if (!hasSelection)
    {
        ImGui::TextWrapped("Select a model in the scene first, then choose a texture to apply through its UVs.");
        return;
    }

    ImGui::TextWrapped(
        "UV Base Color Override: %s",
        model->baseColorTextureOverridePath.empty() ? "<none>" : model->baseColorTextureOverridePath.c_str()
    );

    if (!hasImportedModel)
    {
        ImGui::TextWrapped("The selected entity is still using the built-in cube, so UV texture application is unavailable.");
    }
    else if (!hasUvSubmeshes)
    {
        ImGui::TextWrapped("The current imported model does not expose usable UVs, so the texture button is disabled.");
    }
    else
    {
        ImGui::TextWrapped("The selected texture is sampled as the base color map using the current imported UVs.");
    }
}

bool DrawGraphTextureSlotEditor(
    const char* label,
    const char* idSuffix,
    const std::string& modelPath,
    uint32_t materialIndex,
    const char* slotName,
    std::string& path,
    std::string* statusMessage = nullptr
)
{
    bool changed = false;
    const std::string compactPathLabel = [&path]()
    {
        if (path.empty())
        {
            return std::string("<default input>");
        }

        std::string fileName = std::filesystem::path(path).filename().string();
        if (fileName.empty())
        {
            fileName = path;
        }
        if (fileName.size() <= 26)
        {
            return fileName;
        }

        return fileName.substr(0, 23) + "...";
    }();

    ImGui::PushID(idSuffix);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(0.0f, 10.0f);
    if (path.empty())
    {
        ImGui::TextDisabled("Default");
    }
    else
    {
        ImGui::TextColored(ImVec4(0.82f, 0.88f, 0.96f, 1.0f), "%s", compactPathLabel.c_str());
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", path.c_str());
        }
    }

    if (ImGui::GetContentRegionAvail().x > 120.0f)
    {
        ImGui::SameLine();
    }
    if (ImGui::SmallButton(path.empty() ? "Pick" : "Swap"))
    {
        if (const std::optional<std::string> selectedPath = OpenTextureFileDialog(); selectedPath.has_value())
        {
            try
            {
                path =
                    !modelPath.empty() && slotName != nullptr
                    ? ImportTextureIntoModelMaterialDirectory(modelPath, materialIndex, slotName, *selectedPath)
                    : *selectedPath;
                if (statusMessage != nullptr)
                {
                    *statusMessage = "Imported texture for " + std::string(label) + ": " + path;
                }
                changed = true;
            }
            catch (const std::exception& error)
            {
                if (statusMessage != nullptr)
                {
                    *statusMessage = error.what();
                }
            }
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(path.empty());
    if (ImGui::SmallButton("Clear"))
    {
        path.clear();
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    return changed;
}

size_t CountMaterialGraphSecondaryTextures(const MaterialTextureBlendGraph& blendGraph)
{
    const std::array<const std::string*, 7> paths = {
        &blendGraph.secondaryBaseColorTexturePath,
        &blendGraph.secondaryNormalTexturePath,
        &blendGraph.secondaryMetallicTexturePath,
        &blendGraph.secondaryRoughnessTexturePath,
        &blendGraph.secondaryOcclusionTexturePath,
        &blendGraph.secondaryEmissiveTexturePath,
        &blendGraph.blendMaskTexturePath
    };
    return static_cast<size_t>(std::count_if(paths.begin(), paths.end(), [](const std::string* value)
    {
        return value != nullptr && !value->empty();
    }));
}

struct MaterialGraphNodeFrame
{
    ImRect rect;
    ImRect headerRect;
    ImVec2 inputPin{ 0.0f, 0.0f };
    ImVec2 inputPinAlt{ 0.0f, 0.0f };
    ImVec2 outputPin{ 0.0f, 0.0f };
};

std::array<ImVec2, 4> BuildDefaultMaterialGraphNodeLayout()
{
    return {
        ImVec2(24.0f, 28.0f),
        ImVec2(24.0f, 346.0f),
        ImVec2(392.0f, 182.0f),
        ImVec2(704.0f, 232.0f)
    };
}

void DrawMaterialGraphLegendChip(const char* badge, const char* label, ImU32 color, float uiScale)
{
    const ImVec4 chipColor = ImGui::ColorConvertU32ToFloat4(color);
    ImGui::PushID(label);
    ImGui::PushStyleColor(ImGuiCol_Button, chipColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(
        std::min(chipColor.x + 0.08f, 1.0f),
        std::min(chipColor.y + 0.08f, 1.0f),
        std::min(chipColor.z + 0.08f, 1.0f),
        chipColor.w
    ));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, chipColor);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.97f, 0.98f, 0.99f, 1.0f));
    ImGui::Button(badge, ImVec2(42.0f * uiScale, 0.0f));
    ImGui::PopStyleColor(4);
    ImGui::SameLine(0.0f, 8.0f * uiScale);
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(0.0f, 16.0f * uiScale);
    ImGui::PopID();
}

bool UpdateMaterialGraphNodeDrag(
    int nodeIndex,
    const MaterialGraphNodeFrame& nodeFrame,
    ImVec2& logicalPosition,
    const ImVec2& logicalSize,
    const ImVec2& logicalCanvasSize,
    float uiScale,
    int& draggingNodeIndex
)
{
    const ImVec2 mousePosition = ImGui::GetIO().MousePos;
    const bool hoveredHeader = nodeFrame.headerRect.Contains(mousePosition);
    if (hoveredHeader)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            draggingNodeIndex = nodeIndex;
        }
    }

    if (draggingNodeIndex != nodeIndex)
    {
        return false;
    }

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        draggingNodeIndex = -1;
        return false;
    }

    const float logicalDeltaX = ImGui::GetIO().MouseDelta.x / std::max(uiScale, 0.001f);
    const float logicalDeltaY = ImGui::GetIO().MouseDelta.y / std::max(uiScale, 0.001f);
    logicalPosition.x = std::clamp(
        logicalPosition.x + logicalDeltaX,
        0.0f,
        std::max(logicalCanvasSize.x - logicalSize.x, 0.0f)
    );
    logicalPosition.y = std::clamp(
        logicalPosition.y + logicalDeltaY,
        0.0f,
        std::max(logicalCanvasSize.y - logicalSize.y, 0.0f)
    );
    return std::abs(logicalDeltaX) > 0.0f || std::abs(logicalDeltaY) > 0.0f;
}

MaterialGraphNodeFrame BeginMaterialGraphNode(
    const char* childId,
    const char* badge,
    const char* title,
    const ImVec2& canvasOrigin,
    ImVec2& logicalPosition,
    const ImVec2& logicalSize,
    const ImVec2& logicalCanvasSize,
    float uiScale,
    ImU32 accentColor
)
{
    const ImVec2 screenPosition(
        canvasOrigin.x + logicalPosition.x * uiScale,
        canvasOrigin.y + logicalPosition.y * uiScale
    );
    const ImVec2 size(logicalSize.x * uiScale, logicalSize.y * uiScale);
    const float cornerRounding = 12.0f * uiScale;
    const float headerHeight = 34.0f * uiScale;
    const float contentPaddingX = 12.0f * uiScale;
    const float contentPaddingY = 44.0f * uiScale;

    ImGui::SetCursorScreenPos(screenPosition);
    ImGui::PushID(childId);
    ImGui::BeginChild(
        "NodeFrame",
        size,
        false,
        ImGuiWindowFlags_NoMove
    );

    const ImVec2 nodeMin = ImGui::GetWindowPos();
    const ImVec2 nodeMax(nodeMin.x + size.x, nodeMin.y + size.y);
    const ImRect headerRect(nodeMin, ImVec2(nodeMax.x, nodeMin.y + headerHeight));
    const ImVec2 badgeTextSize = ImGui::CalcTextSize(badge);
    const ImVec2 badgeMin(nodeMin.x + 10.0f * uiScale, nodeMin.y + 7.0f * uiScale);
    const ImVec2 badgeMax(
        badgeMin.x + badgeTextSize.x + 16.0f * uiScale,
        badgeMin.y + std::max(20.0f * uiScale, badgeTextSize.y + 8.0f * uiScale)
    );
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(nodeMin, nodeMax, IM_COL32(17, 21, 28, 245), cornerRounding);
    drawList->AddRect(nodeMin, nodeMax, IM_COL32(76, 90, 108, 255), cornerRounding, 0, 1.5f);
    drawList->AddRectFilled(nodeMin, ImVec2(nodeMax.x, nodeMin.y + headerHeight), accentColor, cornerRounding);
    drawList->AddRectFilled(badgeMin, badgeMax, IM_COL32(245, 248, 252, 220), 8.0f * uiScale);
    drawList->AddText(
        ImVec2(badgeMin.x + 8.0f * uiScale, badgeMin.y + 4.0f * uiScale),
        IM_COL32(34, 38, 44, 255),
        badge
    );
    drawList->AddText(
        ImVec2(badgeMax.x + 10.0f * uiScale, nodeMin.y + 9.0f * uiScale),
        IM_COL32(244, 248, 255, 255),
        title
    );

    ImGui::SetCursorPos(ImVec2(contentPaddingX, contentPaddingY));

    return MaterialGraphNodeFrame{
        ImRect(nodeMin, nodeMax),
        headerRect,
        ImVec2(nodeMin.x, nodeMin.y + size.y * 0.40f),
        ImVec2(nodeMin.x, nodeMin.y + size.y * 0.68f),
        ImVec2(nodeMax.x, nodeMin.y + size.y * 0.52f)
    };
}

void EndMaterialGraphNode()
{
    ImGui::EndChild();
    ImGui::PopID();
}

void DrawMaterialGraphLink(ImDrawList* drawList, const ImVec2& start, const ImVec2& end, ImU32 color, float uiScale)
{
    const float controlOffset = 56.0f * uiScale;
    const float linkThickness = std::max(2.0f * uiScale, 2.0f);
    const float pinRadius = 5.0f * uiScale;
    const ImVec2 controlA(start.x + controlOffset, start.y);
    const ImVec2 controlB(end.x - controlOffset, end.y);
    drawList->AddBezierCubic(start, controlA, controlB, end, color, linkThickness);
    drawList->AddCircleFilled(start, pinRadius, color);
    drawList->AddCircleFilled(end, pinRadius, color);
}

bool IsSupportedModelAssetPath(const std::filesystem::path& path)
{
    return ModelLoader::IsSupportedModelPath(path);
}

bool IsMaterialAssetPath(const std::filesystem::path& path)
{
    const std::string fileName = ToLowerCopy(path.filename().string());
    return
        fileName.size() >= std::string(".material.yaml").size() &&
        fileName.ends_with(".material.yaml");
}

std::string NormalizeAssetPath(const std::filesystem::path& path)
{
    return path.lexically_normal().string();
}

std::filesystem::path NormalizeFilesystemPath(const std::filesystem::path& path)
{
    std::error_code errorCode;
    const std::filesystem::path absolutePath = std::filesystem::absolute(path, errorCode);
    return errorCode ? path.lexically_normal() : absolutePath.lexically_normal();
}

std::filesystem::path BuildImportedAssetManifestPath(const std::filesystem::path& modelPath)
{
    return std::filesystem::path(modelPath.string() + ".miniengine_asset.yaml");
}

std::string BuildImportedMaterialFilePrefix(const std::filesystem::path& modelPath, uint32_t materialIndex)
{
    std::ostringstream prefix;
    prefix << modelPath.stem().string() << "_material_" << std::setfill('0') << std::setw(2) << materialIndex;
    return prefix.str();
}

std::filesystem::path BuildImportedMaterialAssetPath(const std::filesystem::path& modelPath, uint32_t materialIndex)
{
    return modelPath.parent_path() / (BuildImportedMaterialFilePrefix(modelPath, materialIndex) + ".material.yaml");
}

std::filesystem::path BuildImportedMaterialTextureDirectory(const std::filesystem::path& modelPath, uint32_t materialIndex)
{
    static_cast<void>(materialIndex);
    return modelPath.parent_path();
}

void RemoveImportedTextureVariants(const std::filesystem::path& directory, const std::string& slotName)
{
    if (!std::filesystem::exists(directory))
    {
        return;
    }

    std::error_code errorCode;
    for (std::filesystem::directory_iterator iterator(directory, errorCode);
         !errorCode && iterator != std::filesystem::directory_iterator();
         iterator.increment(errorCode))
    {
        if (!iterator->is_regular_file(errorCode) || errorCode)
        {
            continue;
        }

        if (iterator->path().stem() == slotName)
        {
            std::filesystem::remove(iterator->path(), errorCode);
            errorCode.clear();
        }
    }
}

std::vector<ModelImportedMaterialInfo> LoadEffectiveImportedModelMaterials(
    const std::filesystem::path& modelPath,
    const LoadedModelData& loadedModel,
    std::vector<std::string>* materialAssetPaths
)
{
    std::vector<ModelImportedMaterialInfo> materials;
    materials.reserve(loadedModel.materials.size());
    for (const ModelMaterialData& material : loadedModel.materials)
    {
        materials.push_back(BuildImportedMaterialInfo(material));
    }

    if (materials.empty())
    {
        materials.push_back(ModelImportedMaterialInfo{});
    }

    if (materialAssetPaths != nullptr)
    {
        materialAssetPaths->assign(materials.size(), std::string{});
    }

    const std::filesystem::path manifestPath = BuildImportedAssetManifestPath(modelPath);
    if (!std::filesystem::exists(manifestPath))
    {
        return materials;
    }

    try
    {
        const YAML::Node root = YAML::LoadFile(manifestPath.string());
        const YAML::Node materialsNode = root["materials"];
        if (!materialsNode || !materialsNode.IsSequence())
        {
            return materials;
        }

        if (materialsNode.size() > materials.size())
        {
            materials.resize(materialsNode.size());
            if (materialAssetPaths != nullptr)
            {
                materialAssetPaths->resize(materialsNode.size());
            }
        }

        for (size_t materialIndex = 0; materialIndex < materialsNode.size(); ++materialIndex)
        {
            const YAML::Node materialNode = materialsNode[materialIndex];
            if (!materialNode || !materialNode.IsMap())
            {
                continue;
            }

            const std::string fallbackName = BuildMaterialSlotLabel(materials[materialIndex], materialIndex);
            materials[materialIndex] = ReadImportedMaterialInfoFromYamlNode(materialNode, fallbackName);

            if (materialAssetPaths != nullptr)
            {
                const YAML::Node materialAssetPathNode = materialNode["material_asset_path"];
                if (materialAssetPathNode && materialAssetPathNode.IsScalar())
                {
                    (*materialAssetPaths)[materialIndex] = materialAssetPathNode.as<std::string>();
                }
            }
        }
    }
    catch (...)
    {
        // Keep the editor usable even if metadata is malformed.
    }

    return materials;
}

bool LoadMaterialPreviewAsset(
    const std::filesystem::path& materialPath,
    ModelImportedMaterialInfo& material,
    std::string& modelPath,
    int& materialIndex,
    std::string& statusMessage
)
{
    try
    {
        const YAML::Node root = YAML::LoadFile(materialPath.string());
        const YAML::Node assetNode = root["asset"];
        const std::string fallbackName = materialPath.stem().stem().string().empty()
            ? "Material"
            : materialPath.stem().stem().string();
        material = ReadImportedMaterialInfoFromYamlNode(root["material"], fallbackName);
        modelPath = assetNode["model_path"].as<std::string>(std::string{});
        materialIndex = assetNode["material_index"].as<int>(0);
        statusMessage.clear();
        return true;
    }
    catch (const std::exception& error)
    {
        material = ModelImportedMaterialInfo{};
        modelPath.clear();
        materialIndex = 0;
        statusMessage = error.what();
        return false;
    }
}

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
    const ImVec2& canvasSize
)
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

void DrawModelWireframePreview(
    const LoadedModelData& loadedModel,
    float& yaw,
    float& pitch,
    float& distance,
    bool& autoFramePending,
    float uiScale
)
{
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 canvasSize(
        std::max(available.x, 220.0f * uiScale),
        std::max(240.0f * uiScale, std::min(available.x * 0.62f, 360.0f * uiScale))
    );

    ImGui::InvisibleButton("ModelWireframeCanvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
    const ImVec2 canvasMin = ImGui::GetItemRectMin();
    const ImVec2 canvasMax = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(canvasMin, canvasMax, IM_COL32(13, 18, 26, 255), 10.0f * uiScale);
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

    const glm::vec3 center = ComputeLoadedModelCenter(loadedModel);
    const float radius = ComputeLoadedModelRadius(loadedModel, center);
    if (autoFramePending)
    {
        distance = std::max(radius * 2.75f, 0.8f);
        autoFramePending = false;
    }

    const glm::vec3 viewDirection(
        std::cos(pitch) * std::sin(yaw),
        std::sin(pitch),
        std::cos(pitch) * std::cos(yaw)
    );
    const glm::vec3 eye = center + viewDirection * distance;
    const glm::mat4 view = glm::lookAt(eye, center, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 projection = glm::perspective(
        glm::radians(45.0f),
        std::max(canvasSize.x / std::max(canvasSize.y, 1.0f), 0.1f),
        0.01f,
        std::max(radius * 10.0f + distance, 10.0f)
    );
    const glm::mat4 viewProjection = projection * view;

    const ImU32 axisColor = IM_COL32(70, 88, 110, 255);
    const ImVec2 axisCenter(canvasMin.x + 66.0f * uiScale, canvasMax.y - 54.0f * uiScale);
    drawList->AddLine(
        ImVec2(axisCenter.x - 18.0f * uiScale, axisCenter.y),
        ImVec2(axisCenter.x + 18.0f * uiScale, axisCenter.y),
        axisColor,
        1.0f
    );
    drawList->AddLine(
        ImVec2(axisCenter.x, axisCenter.y - 18.0f * uiScale),
        ImVec2(axisCenter.x, axisCenter.y + 18.0f * uiScale),
        axisColor,
        1.0f
    );

    constexpr std::array<ImU32, 6> kSubmeshColors = {
        IM_COL32(115, 196, 255, 255),
        IM_COL32(255, 191, 107, 255),
        IM_COL32(132, 226, 176, 255),
        IM_COL32(255, 143, 163, 255),
        IM_COL32(198, 171, 255, 255),
        IM_COL32(255, 234, 130, 255)
    };

    for (size_t submeshIndex = 0; submeshIndex < loadedModel.submeshes.size(); ++submeshIndex)
    {
        const ModelSubmeshData& submesh = loadedModel.submeshes[submeshIndex];
        if (!submesh.mesh.IsValid())
        {
            continue;
        }

        const ImU32 lineColor = kSubmeshColors[submeshIndex % kSubmeshColors.size()];
        for (size_t index = 0; index + 2 < submesh.mesh.indices.size(); index += 3)
        {
            const uint32_t index0 = submesh.mesh.indices[index];
            const uint32_t index1 = submesh.mesh.indices[index + 1];
            const uint32_t index2 = submesh.mesh.indices[index + 2];
            if (index0 >= submesh.mesh.vertices.size() ||
                index1 >= submesh.mesh.vertices.size() ||
                index2 >= submesh.mesh.vertices.size())
            {
                continue;
            }

            const Vertex& vertex0 = submesh.mesh.vertices[index0];
            const Vertex& vertex1 = submesh.mesh.vertices[index1];
            const Vertex& vertex2 = submesh.mesh.vertices[index2];
            const auto point0 = ProjectPreviewPoint(
                glm::vec3(vertex0.position[0], vertex0.position[1], vertex0.position[2]),
                viewProjection,
                canvasMin,
                canvasSize
            );
            const auto point1 = ProjectPreviewPoint(
                glm::vec3(vertex1.position[0], vertex1.position[1], vertex1.position[2]),
                viewProjection,
                canvasMin,
                canvasSize
            );
            const auto point2 = ProjectPreviewPoint(
                glm::vec3(vertex2.position[0], vertex2.position[1], vertex2.position[2]),
                viewProjection,
                canvasMin,
                canvasSize
            );
            if (!point0.has_value() || !point1.has_value() || !point2.has_value())
            {
                continue;
            }

            drawList->AddLine(*point0, *point1, lineColor, 1.3f);
            drawList->AddLine(*point1, *point2, lineColor, 1.3f);
            drawList->AddLine(*point2, *point0, lineColor, 1.3f);
        }
    }

    drawList->AddText(
        ImVec2(canvasMin.x + 12.0f * uiScale, canvasMin.y + 12.0f * uiScale),
        IM_COL32(210, 220, 236, 255),
        "Drag to orbit, wheel to zoom"
    );
}

void DrawModelUvPreview(
    const LoadedModelData& loadedModel,
    int& selectedUvSubmeshIndex,
    float uiScale
)
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
        "UV 0-1 space"
    );
    drawList->AddText(
        ImVec2(canvasMin.x + 12.0f * uiScale, canvasMax.y - 24.0f * uiScale),
        IM_COL32(170, 182, 204, 255),
        cappedUvPreview ? "Sampled wireframe preview for responsiveness" : "Full UV wireframe preview"
    );
}

struct CachedPreviewTexture
{
    TextureData texture;
    std::filesystem::file_time_type lastWriteTime{};
    bool resolved = false;
    bool available = false;
};

struct PreviewSurfaceVertex
{
    glm::vec3 position{ 0.0f };
    glm::vec3 color{ 1.0f };
    glm::vec2 uv{ 0.0f };
    glm::vec3 normal{ 0.0f, 0.0f, 1.0f };
    glm::vec4 tangent{ 1.0f, 0.0f, 0.0f, 1.0f };
};

struct PreviewRasterVertex
{
    ImVec2 screen{ 0.0f, 0.0f };
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

void ResetMaterialShadedPreviewCache(const char* canvasId)
{
    if (canvasId == nullptr)
    {
        return;
    }

    GetMaterialShadedPreviewCaches().erase(canvasId);
}

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
    size_t materialSignature
)
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
    size_t materialSignature
)
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
            cached.texture = TextureLoader::LoadRGBA8(normalizedPath.string(), true);
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
        std::pow(clamped.b, 1.0f / 2.2f)
    );
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
            static_cast<float>(texture.pixels[offset + 3]) / 255.0f
        );
    };

    const glm::vec4 top = glm::mix(fetchPixel(x0, y0), fetchPixel(x1, y0), tx);
    const glm::vec4 bottom = glm::mix(fetchPixel(x0, y1), fetchPixel(x1, y1), tx);
    return glm::mix(top, bottom, ty);
}

glm::vec4 SamplePreviewTexture(
    const std::string& path,
    const glm::vec2& uv,
    const glm::vec4& fallback,
    bool srgb
)
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
    float barycentricC
)
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
    const glm::vec3& cameraPosition
)
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
        true
    );
    const glm::vec4 primaryNormal =
        SamplePreviewTexture(material.normalTexturePath, vertex.uv, flatNormal, false);
    const glm::vec4 secondaryNormal = SamplePreviewTexture(
        material.blendGraph.secondaryNormalTexturePath.empty()
            ? material.normalTexturePath
            : material.blendGraph.secondaryNormalTexturePath,
        vertex.uv,
        primaryNormal,
        false
    );
    const glm::vec4 primaryMetallic =
        SamplePreviewTexture(material.metallicTexturePath, vertex.uv, whiteLinear, false);
    const glm::vec4 secondaryMetallic = SamplePreviewTexture(
        material.blendGraph.secondaryMetallicTexturePath.empty()
            ? material.metallicTexturePath
            : material.blendGraph.secondaryMetallicTexturePath,
        vertex.uv,
        primaryMetallic,
        false
    );
    const glm::vec4 primaryRoughness =
        SamplePreviewTexture(material.roughnessTexturePath, vertex.uv, whiteLinear, false);
    const glm::vec4 secondaryRoughness = SamplePreviewTexture(
        material.blendGraph.secondaryRoughnessTexturePath.empty()
            ? material.roughnessTexturePath
            : material.blendGraph.secondaryRoughnessTexturePath,
        vertex.uv,
        primaryRoughness,
        false
    );
    const glm::vec4 primaryOcclusion =
        SamplePreviewTexture(material.occlusionTexturePath, vertex.uv, whiteLinear, false);
    const glm::vec4 secondaryOcclusion = SamplePreviewTexture(
        material.blendGraph.secondaryOcclusionTexturePath.empty()
            ? material.occlusionTexturePath
            : material.blendGraph.secondaryOcclusionTexturePath,
        vertex.uv,
        primaryOcclusion,
        false
    );
    const glm::vec4 primaryEmissive =
        SamplePreviewTexture(material.emissiveTexturePath, vertex.uv, whiteLinear, true);
    const glm::vec4 secondaryEmissive = SamplePreviewTexture(
        material.blendGraph.secondaryEmissiveTexturePath.empty()
            ? material.emissiveTexturePath
            : material.blendGraph.secondaryEmissiveTexturePath,
        vertex.uv,
        primaryEmissive,
        true
    );
    const float blendMask = SamplePreviewTexture(
        material.blendGraph.blendMaskTexturePath,
        vertex.uv,
        whiteLinear,
        false
    ).r;

    const float blendWeight = glm::clamp(
        (material.blendGraph.enabled ? material.blendGraph.blendFactor : 0.0f) * blendMask,
        0.0f,
        1.0f
    );

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
            geometricNormal
        ));
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
        blendWeight
    );

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
        static_cast<int>(std::round(clamped.a * 255.0f))
    );
}

void AddGradientTriangle(
    ImDrawList* drawList,
    const PreviewRasterVertex& a,
    const PreviewRasterVertex& b,
    const PreviewRasterVertex& c
)
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
    float distance
)
{
    const glm::vec3 center = ComputeLoadedModelCenter(loadedModel);
    const float radius = ComputeLoadedModelRadius(loadedModel, center);
    const glm::vec3 viewDirection(
        std::cos(pitch) * std::sin(yaw),
        std::sin(pitch),
        std::cos(pitch) * std::cos(yaw)
    );
    const glm::vec3 eye = center + viewDirection * distance;
    const glm::mat4 view = glm::lookAt(eye, center, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 projection = glm::perspective(
        glm::radians(45.0f),
        std::max(canvasSize.x / std::max(canvasSize.y, 1.0f), 0.1f),
        0.01f,
        std::max(radius * 10.0f + distance, 10.0f)
    );
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
        const PreviewSurfaceVertex inputVertices[3] = { a, b, c };
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
                {
                    glm::vec3(rawVertex0.position[0], rawVertex0.position[1], rawVertex0.position[2]),
                    glm::vec3(rawVertex0.color[0], rawVertex0.color[1], rawVertex0.color[2]),
                    glm::vec2(rawVertex0.texCoord[0], rawVertex0.texCoord[1]),
                    glm::vec3(rawVertex0.normal[0], rawVertex0.normal[1], rawVertex0.normal[2]),
                    glm::vec4(rawVertex0.tangent[0], rawVertex0.tangent[1], rawVertex0.tangent[2], rawVertex0.tangent[3])
                },
                {
                    glm::vec3(rawVertex1.position[0], rawVertex1.position[1], rawVertex1.position[2]),
                    glm::vec3(rawVertex1.color[0], rawVertex1.color[1], rawVertex1.color[2]),
                    glm::vec2(rawVertex1.texCoord[0], rawVertex1.texCoord[1]),
                    glm::vec3(rawVertex1.normal[0], rawVertex1.normal[1], rawVertex1.normal[2]),
                    glm::vec4(rawVertex1.tangent[0], rawVertex1.tangent[1], rawVertex1.tangent[2], rawVertex1.tangent[3])
                },
                {
                    glm::vec3(rawVertex2.position[0], rawVertex2.position[1], rawVertex2.position[2]),
                    glm::vec3(rawVertex2.color[0], rawVertex2.color[1], rawVertex2.color[2]),
                    glm::vec2(rawVertex2.texCoord[0], rawVertex2.texCoord[1]),
                    glm::vec3(rawVertex2.normal[0], rawVertex2.normal[1], rawVertex2.normal[2]),
                    glm::vec4(rawVertex2.tangent[0], rawVertex2.tangent[1], rawVertex2.tangent[2], rawVertex2.tangent[3])
                }
            };

            const auto projected0 = ProjectPreviewPoint(baseVertices[0].position, viewProjection, canvasMin, canvasSize);
            const auto projected1 = ProjectPreviewPoint(baseVertices[1].position, viewProjection, canvasMin, canvasSize);
            const auto projected2 = ProjectPreviewPoint(baseVertices[2].position, viewProjection, canvasMin, canvasSize);
            if (!projected0.has_value() || !projected1.has_value() || !projected2.has_value())
            {
                continue;
            }

            const float triangleArea = std::abs(
                ((*projected1).x - (*projected0).x) * ((*projected2).y - (*projected0).y) -
                ((*projected2).x - (*projected0).x) * ((*projected1).y - (*projected0).y)
            ) * 0.5f;
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
                        barycentricC
                    ));
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
    const char* overlayLabel
)
{
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 canvasSize(
        std::max(available.x, 260.0f * uiScale),
        std::max(260.0f * uiScale, std::min(available.y, 360.0f * uiScale))
    );

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
        IM_COL32(18, 20, 30, 255)
    );
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
            "Unable to preview this model."
        );
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
            distance
        );
        StoreMaterialShadedPreviewCacheKey(
            previewCache,
            canvasMin,
            canvasSize,
            selectedMaterialIndex,
            yaw,
            pitch,
            distance,
            materialSignature
        );
    }

    for (const PreviewTriangle& triangle : previewCache.triangles)
    {
        AddGradientTriangle(drawList, triangle.vertices[0], triangle.vertices[1], triangle.vertices[2]);
    }

    drawList->AddText(
        ImVec2(canvasMin.x + 12.0f * uiScale, canvasMin.y + 12.0f * uiScale),
        IM_COL32(218, 224, 236, 255),
        overlayLabel
    );
    drawList->AddText(
        ImVec2(canvasMin.x + 12.0f * uiScale, canvasMax.y - 26.0f * uiScale),
        IM_COL32(176, 188, 206, 255),
        previewCache.cappedTriangles ? "Approximate preview capped for responsiveness" : "Drag to orbit, wheel to zoom"
    );
}

enum class MaterialGraphPinKind : uint32_t
{
    Texture = 0,
    Scalar = 1,
    Color = 2,
    Surface = 3
};

struct MaterialGraphPinDefinition
{
    const char* slot = "";
    const char* label = "";
    MaterialGraphPinKind kind = MaterialGraphPinKind::Texture;
};

struct MaterialGraphRenderedPin
{
    uint32_t nodeId = 0;
    std::string slot;
    MaterialGraphPinKind kind = MaterialGraphPinKind::Texture;
    bool input = true;
    ImVec2 center{ 0.0f, 0.0f };
    uint32_t linkId = 0;
};

struct MaterialGraphNodeDrawResult
{
    bool changed = false;
    bool selected = false;
    bool hovered = false;
    bool capturesMouse = false;
    bool requestCopy = false;
    bool requestDelete = false;
    bool requestPaste = false;
    bool requestStartResize = false;
    bool requestStartLinkDrag = false;
    uint32_t startLinkNodeId = 0;
    uint32_t selectedLinkId = 0;
    uint32_t connectedLinkId = 0;
    uint8_t resizeEdges = 0;
    std::string startLinkSlot;
    std::vector<MaterialGraphRenderedPin> pins;
    ImVec2 min{ 0.0f, 0.0f };
    ImVec2 max{ 0.0f, 0.0f };
    MaterialGraphNodePosition pastePosition{};
};

enum MaterialGraphResizeEdgeFlags : uint8_t
{
    MaterialGraphResizeEdge_None = 0,
    MaterialGraphResizeEdge_Left = 1 << 0,
    MaterialGraphResizeEdge_Right = 1 << 1,
    MaterialGraphResizeEdge_Top = 1 << 2,
    MaterialGraphResizeEdge_Bottom = 1 << 3
};

const char* GetMaterialGraphNodeTypeLabel(MaterialShaderNodeType type)
{
    switch (type)
    {
    case MaterialShaderNodeType::Texture:
        return "Texture";
    case MaterialShaderNodeType::Scalar:
        return "Scalar";
    case MaterialShaderNodeType::Color:
        return "Color";
    case MaterialShaderNodeType::Surface:
        return "Surface";
    case MaterialShaderNodeType::Blend:
        return "Blend";
    case MaterialShaderNodeType::Output:
        return "Output";
    default:
        return "Node";
    }
}

const char* GetDefaultMaterialGraphNodeName(MaterialShaderNodeType type)
{
    switch (type)
    {
    case MaterialShaderNodeType::Texture:
        return "Texture";
    case MaterialShaderNodeType::Scalar:
        return "Scalar";
    case MaterialShaderNodeType::Color:
        return "Color";
    case MaterialShaderNodeType::Surface:
        return "Surface";
    case MaterialShaderNodeType::Blend:
        return "Blend";
    case MaterialShaderNodeType::Output:
        return "Material Output";
    default:
        return "Node";
    }
}

ImVec4 ScaleColor(const ImVec4& color, float factor)
{
    return ImVec4(
        std::clamp(color.x * factor, 0.0f, 1.0f),
        std::clamp(color.y * factor, 0.0f, 1.0f),
        std::clamp(color.z * factor, 0.0f, 1.0f),
        color.w
    );
}

ImVec4 GetMaterialGraphHeaderColor(MaterialShaderNodeType type)
{
    switch (type)
    {
    case MaterialShaderNodeType::Texture:
        return ImVec4(0.22f, 0.38f, 0.64f, 1.0f);
    case MaterialShaderNodeType::Scalar:
        return ImVec4(0.48f, 0.33f, 0.17f, 1.0f);
    case MaterialShaderNodeType::Color:
        return ImVec4(0.48f, 0.20f, 0.26f, 1.0f);
    case MaterialShaderNodeType::Surface:
        return ImVec4(0.19f, 0.45f, 0.34f, 1.0f);
    case MaterialShaderNodeType::Blend:
        return ImVec4(0.47f, 0.36f, 0.17f, 1.0f);
    case MaterialShaderNodeType::Output:
        return ImVec4(0.31f, 0.28f, 0.56f, 1.0f);
    default:
        return ImVec4(0.20f, 0.24f, 0.30f, 1.0f);
    }
}

ImU32 GetMaterialGraphPinColor(MaterialGraphPinKind kind)
{
    switch (kind)
    {
    case MaterialGraphPinKind::Texture:
        return IM_COL32(108, 182, 255, 255);
    case MaterialGraphPinKind::Scalar:
        return IM_COL32(255, 188, 102, 255);
    case MaterialGraphPinKind::Color:
        return IM_COL32(244, 132, 132, 255);
    case MaterialGraphPinKind::Surface:
        return IM_COL32(170, 220, 168, 255);
    default:
        return IM_COL32(196, 204, 218, 255);
    }
}

const std::vector<MaterialGraphPinDefinition>& GetMaterialGraphInputPins(MaterialShaderNodeType type)
{
    static const std::vector<MaterialGraphPinDefinition> kEmptyPins{};
    static const std::vector<MaterialGraphPinDefinition> kSurfacePins = {
        { "base_color", "Base Color", MaterialGraphPinKind::Texture },
        { "normal", "Normal", MaterialGraphPinKind::Texture },
        { "metallic", "Metallic", MaterialGraphPinKind::Texture },
        { "roughness", "Roughness", MaterialGraphPinKind::Texture },
        { "occlusion", "Occlusion", MaterialGraphPinKind::Texture },
        { "emissive", "Emissive", MaterialGraphPinKind::Texture }
    };
    static const std::vector<MaterialGraphPinDefinition> kBlendPins = {
        { "surface_a", "Surface A", MaterialGraphPinKind::Surface },
        { "surface_b", "Surface B", MaterialGraphPinKind::Surface },
        { "mask", "Mask", MaterialGraphPinKind::Texture },
        { "factor", "Factor", MaterialGraphPinKind::Scalar }
    };
    static const std::vector<MaterialGraphPinDefinition> kOutputPins = {
        { "surface", "Surface", MaterialGraphPinKind::Surface },
        { "base_factor", "Base Factor", MaterialGraphPinKind::Color },
        { "metallic_factor", "Metallic", MaterialGraphPinKind::Scalar },
        { "roughness_factor", "Roughness", MaterialGraphPinKind::Scalar },
        { "normal_scale", "Normal Scale", MaterialGraphPinKind::Scalar },
        { "ao_strength", "AO Strength", MaterialGraphPinKind::Scalar },
        { "emissive_color", "Emissive", MaterialGraphPinKind::Color },
        { "emissive_intensity", "Emissive Intensity", MaterialGraphPinKind::Scalar },
        { "opacity", "Opacity", MaterialGraphPinKind::Scalar }
    };

    switch (type)
    {
    case MaterialShaderNodeType::Surface:
        return kSurfacePins;
    case MaterialShaderNodeType::Blend:
        return kBlendPins;
    case MaterialShaderNodeType::Output:
        return kOutputPins;
    default:
        return kEmptyPins;
    }
}

const std::vector<MaterialGraphPinDefinition>& GetMaterialGraphOutputPins(MaterialShaderNodeType type)
{
    static const std::vector<MaterialGraphPinDefinition> kEmptyPins{};
    static const std::vector<MaterialGraphPinDefinition> kTexturePins = {
        { "texture", "Texture", MaterialGraphPinKind::Texture }
    };
    static const std::vector<MaterialGraphPinDefinition> kScalarPins = {
        { "value", "Value", MaterialGraphPinKind::Scalar }
    };
    static const std::vector<MaterialGraphPinDefinition> kColorPins = {
        { "color", "Color", MaterialGraphPinKind::Color }
    };
    static const std::vector<MaterialGraphPinDefinition> kSurfacePins = {
        { "surface", "Surface", MaterialGraphPinKind::Surface }
    };

    switch (type)
    {
    case MaterialShaderNodeType::Texture:
        return kTexturePins;
    case MaterialShaderNodeType::Scalar:
        return kScalarPins;
    case MaterialShaderNodeType::Color:
        return kColorPins;
    case MaterialShaderNodeType::Surface:
    case MaterialShaderNodeType::Blend:
        return kSurfacePins;
    default:
        return kEmptyPins;
    }
}

const MaterialGraphPinDefinition* FindMaterialGraphPinDefinition(
    const std::vector<MaterialGraphPinDefinition>& pins,
    std::string_view slot
)
{
    const auto iterator = std::find_if(pins.begin(), pins.end(), [slot](const MaterialGraphPinDefinition& pin)
    {
        return pin.slot == slot;
    });
    return iterator != pins.end() ? &(*iterator) : nullptr;
}

const MaterialShaderNode* FindMaterialGraphNode(const MaterialShaderGraph& graph, uint32_t nodeId)
{
    const auto iterator = std::find_if(graph.nodes.begin(), graph.nodes.end(), [nodeId](const MaterialShaderNode& node)
    {
        return node.id == nodeId;
    });
    return iterator != graph.nodes.end() ? &(*iterator) : nullptr;
}

MaterialShaderNode* FindMaterialGraphNode(MaterialShaderGraph& graph, uint32_t nodeId)
{
    const auto iterator = std::find_if(graph.nodes.begin(), graph.nodes.end(), [nodeId](const MaterialShaderNode& node)
    {
        return node.id == nodeId;
    });
    return iterator != graph.nodes.end() ? &(*iterator) : nullptr;
}

const MaterialShaderLink* FindMaterialGraphLink(const MaterialShaderGraph& graph, uint32_t linkId)
{
    const auto iterator = std::find_if(graph.links.begin(), graph.links.end(), [linkId](const MaterialShaderLink& link)
    {
        return link.id == linkId;
    });
    return iterator != graph.links.end() ? &(*iterator) : nullptr;
}

const MaterialShaderLink* FindIncomingMaterialGraphLink(
    const MaterialShaderGraph& graph,
    uint32_t nodeId,
    std::string_view slot
)
{
    const auto iterator = std::find_if(graph.links.begin(), graph.links.end(), [&](const MaterialShaderLink& link)
    {
        return link.toNodeId == nodeId && link.toSlot == slot;
    });
    return iterator != graph.links.end() ? &(*iterator) : nullptr;
}

const MaterialGraphRenderedPin* FindRenderedMaterialGraphPin(
    const std::vector<MaterialGraphRenderedPin>& pins,
    uint32_t nodeId,
    std::string_view slot,
    bool input
)
{
    const auto iterator = std::find_if(pins.begin(), pins.end(), [&](const MaterialGraphRenderedPin& pin)
    {
        return pin.nodeId == nodeId && pin.input == input && pin.slot == slot;
    });
    return iterator != pins.end() ? &(*iterator) : nullptr;
}

bool MaterialGraphHasOutputNode(const MaterialShaderGraph& graph)
{
    return std::any_of(graph.nodes.begin(), graph.nodes.end(), [](const MaterialShaderNode& node)
    {
        return node.type == MaterialShaderNodeType::Output;
    });
}

std::string BuildMaterialGraphNodeName(MaterialShaderNodeType type, uint32_t nodeId)
{
    if (type == MaterialShaderNodeType::Output)
    {
        return "Material Output";
    }
    return std::string(GetDefaultMaterialGraphNodeName(type)) + " " + std::to_string(nodeId);
}

MaterialShaderNode* AddMaterialGraphNode(
    MaterialShaderGraph& graph,
    MaterialShaderNodeType type,
    const MaterialGraphNodePosition& position
)
{
    MaterialShaderNode node{};
    node.id = graph.nextNodeId++;
    node.type = type;
    node.name = BuildMaterialGraphNodeName(type, node.id);
    node.position = position;
    if (type == MaterialShaderNodeType::Blend)
    {
        node.scalarValue = 0.5f;
    }
    graph.nodes.push_back(node);
    return &graph.nodes.back();
}

void RemoveMaterialGraphLink(MaterialShaderGraph& graph, uint32_t linkId)
{
    graph.links.erase(
        std::remove_if(
            graph.links.begin(),
            graph.links.end(),
            [linkId](const MaterialShaderLink& link)
            {
                return link.id == linkId;
            }
        ),
        graph.links.end()
    );
}

void RemoveMaterialGraphIncomingLink(
    MaterialShaderGraph& graph,
    uint32_t nodeId,
    std::string_view slot
)
{
    graph.links.erase(
        std::remove_if(
            graph.links.begin(),
            graph.links.end(),
            [nodeId, slot](const MaterialShaderLink& link)
            {
                return link.toNodeId == nodeId && link.toSlot == slot;
            }
        ),
        graph.links.end()
    );
}

void RemoveMaterialGraphNode(MaterialShaderGraph& graph, uint32_t nodeId)
{
    graph.links.erase(
        std::remove_if(
            graph.links.begin(),
            graph.links.end(),
            [nodeId](const MaterialShaderLink& link)
            {
                return link.fromNodeId == nodeId || link.toNodeId == nodeId;
            }
        ),
        graph.links.end()
    );
    graph.nodes.erase(
        std::remove_if(
            graph.nodes.begin(),
            graph.nodes.end(),
            [nodeId](const MaterialShaderNode& node)
            {
                return node.id == nodeId;
            }
        ),
        graph.nodes.end()
    );
}

bool WouldMaterialGraphCreateCycle(
    const MaterialShaderGraph& graph,
    uint32_t fromNodeId,
    uint32_t toNodeId
)
{
    std::vector<uint32_t> pending{ toNodeId };
    std::vector<uint32_t> visited;

    while (!pending.empty())
    {
        const uint32_t currentNodeId = pending.back();
        pending.pop_back();

        if (currentNodeId == fromNodeId)
        {
            return true;
        }

        if (std::find(visited.begin(), visited.end(), currentNodeId) != visited.end())
        {
            continue;
        }
        visited.push_back(currentNodeId);

        for (const MaterialShaderLink& link : graph.links)
        {
            if (link.fromNodeId == currentNodeId)
            {
                pending.push_back(link.toNodeId);
            }
        }
    }

    return false;
}

bool CanConnectMaterialGraphPins(
    const MaterialShaderGraph& graph,
    uint32_t fromNodeId,
    std::string_view fromSlot,
    uint32_t toNodeId,
    std::string_view toSlot,
    std::string* failureReason = nullptr
)
{
    const MaterialShaderNode* fromNode = FindMaterialGraphNode(graph, fromNodeId);
    const MaterialShaderNode* toNode = FindMaterialGraphNode(graph, toNodeId);
    if (fromNode == nullptr || toNode == nullptr)
    {
        if (failureReason != nullptr)
        {
            *failureReason = "The selected node pin is no longer available.";
        }
        return false;
    }

    if (fromNodeId == toNodeId)
    {
        if (failureReason != nullptr)
        {
            *failureReason = "A node cannot be connected to itself.";
        }
        return false;
    }

    const MaterialGraphPinDefinition* outputPin =
        FindMaterialGraphPinDefinition(GetMaterialGraphOutputPins(fromNode->type), fromSlot);
    const MaterialGraphPinDefinition* inputPin =
        FindMaterialGraphPinDefinition(GetMaterialGraphInputPins(toNode->type), toSlot);
    if (outputPin == nullptr || inputPin == nullptr)
    {
        if (failureReason != nullptr)
        {
            *failureReason = "That connection uses an invalid pin.";
        }
        return false;
    }

    if (outputPin->kind != inputPin->kind)
    {
        if (failureReason != nullptr)
        {
            *failureReason = "Only pins with the same data type can be connected.";
        }
        return false;
    }

    if (WouldMaterialGraphCreateCycle(graph, fromNodeId, toNodeId))
    {
        if (failureReason != nullptr)
        {
            *failureReason = "That connection would create a cycle in the material graph.";
        }
        return false;
    }

    return true;
}

uint32_t ConnectMaterialGraphPins(
    MaterialShaderGraph& graph,
    uint32_t fromNodeId,
    std::string_view fromSlot,
    uint32_t toNodeId,
    std::string_view toSlot
)
{
    if (!CanConnectMaterialGraphPins(graph, fromNodeId, fromSlot, toNodeId, toSlot))
    {
        return 0;
    }

    for (const MaterialShaderLink& existingLink : graph.links)
    {
        if (existingLink.fromNodeId == fromNodeId &&
            existingLink.fromSlot == fromSlot &&
            existingLink.toNodeId == toNodeId &&
            existingLink.toSlot == toSlot)
        {
            return existingLink.id;
        }
    }

    RemoveMaterialGraphIncomingLink(graph, toNodeId, toSlot);

    MaterialShaderLink link{};
    link.id = graph.nextLinkId++;
    link.fromNodeId = fromNodeId;
    link.fromSlot = std::string(fromSlot);
    link.toNodeId = toNodeId;
    link.toSlot = std::string(toSlot);
    graph.links.push_back(link);
    return link.id;
}

ImVec2 ComputeNodeScreenPosition(
    const MaterialGraphNodePosition& position,
    const ImVec2& canvasOrigin,
    const MaterialGraphNodePosition& viewOrigin,
    float zoom
)
{
    return ImVec2(
        canvasOrigin.x + (position.x - viewOrigin.x) * zoom,
        canvasOrigin.y + (position.y - viewOrigin.y) * zoom
    );
}

MaterialGraphNodePosition ComputeMaterialGraphPositionFromScreen(
    const ImVec2& screenPosition,
    const ImVec2& canvasOrigin,
    const MaterialGraphNodePosition& viewOrigin,
    float zoom
)
{
    return MaterialGraphNodePosition{
        viewOrigin.x + (screenPosition.x - canvasOrigin.x) / zoom,
        viewOrigin.y + (screenPosition.y - canvasOrigin.y) / zoom
    };
}

ImVec2 GetMaterialGraphNodeBaseSize(MaterialShaderNodeType type)
{
    switch (type)
    {
    case MaterialShaderNodeType::Texture:
        return ImVec2(320.0f, 140.0f);
    case MaterialShaderNodeType::Scalar:
        return ImVec2(240.0f, 132.0f);
    case MaterialShaderNodeType::Color:
        return ImVec2(260.0f, 152.0f);
    case MaterialShaderNodeType::Surface:
        return ImVec2(280.0f, 310.0f);
    case MaterialShaderNodeType::Blend:
        return ImVec2(280.0f, 220.0f);
    case MaterialShaderNodeType::Output:
        return ImVec2(360.0f, 410.0f);
    default:
        return ImVec2(280.0f, 180.0f);
    }
}

ImVec2 GetMaterialGraphNodeMinSize(MaterialShaderNodeType type)
{
    switch (type)
    {
    case MaterialShaderNodeType::Texture:
        return ImVec2(220.0f, 128.0f);
    case MaterialShaderNodeType::Scalar:
        return ImVec2(180.0f, 124.0f);
    case MaterialShaderNodeType::Color:
        return ImVec2(200.0f, 140.0f);
    case MaterialShaderNodeType::Surface:
        return ImVec2(240.0f, 260.0f);
    case MaterialShaderNodeType::Blend:
        return ImVec2(220.0f, 188.0f);
    case MaterialShaderNodeType::Output:
        return ImVec2(300.0f, 340.0f);
    default:
        return ImVec2(200.0f, 140.0f);
    }
}

ImVec2 GetMaterialGraphNodeLogicalSize(const MaterialShaderNode& node)
{
    const ImVec2 baseSize = GetMaterialGraphNodeBaseSize(node.type);
    return ImVec2(
        node.width > 0.0f ? node.width : baseSize.x,
        node.height > 0.0f ? node.height : baseSize.y
    );
}

void SetMaterialGraphNodeLogicalSize(MaterialShaderNode& node, const ImVec2& logicalSize)
{
    const ImVec2 baseSize = GetMaterialGraphNodeBaseSize(node.type);
    node.width = std::abs(logicalSize.x - baseSize.x) <= 0.01f ? 0.0f : logicalSize.x;
    node.height = std::abs(logicalSize.y - baseSize.y) <= 0.01f ? 0.0f : logicalSize.y;
}

ImVec2 GetMaterialGraphNodeSize(const MaterialShaderNode& node, float uiScale)
{
    const ImVec2 logicalSize = GetMaterialGraphNodeLogicalSize(node);
    return ImVec2(logicalSize.x * uiScale, logicalSize.y * uiScale);
}

uint8_t GetMaterialGraphResizeEdges(const ImRect& nodeRect, const ImVec2& mousePosition, float uiScale)
{
    if (!nodeRect.Contains(mousePosition))
    {
        return MaterialGraphResizeEdge_None;
    }

    const float borderThickness = kMaterialGraphResizeBorderPixels * uiScale;
    const ImRect innerRect(
        ImVec2(nodeRect.Min.x + borderThickness, nodeRect.Min.y + borderThickness),
        ImVec2(nodeRect.Max.x - borderThickness, nodeRect.Max.y - borderThickness)
    );
    if (innerRect.Contains(mousePosition))
    {
        return MaterialGraphResizeEdge_None;
    }

    uint8_t edges = MaterialGraphResizeEdge_None;
    if (mousePosition.x <= nodeRect.Min.x + borderThickness)
    {
        edges |= MaterialGraphResizeEdge_Left;
    }
    else if (mousePosition.x >= nodeRect.Max.x - borderThickness)
    {
        edges |= MaterialGraphResizeEdge_Right;
    }

    if (mousePosition.y <= nodeRect.Min.y + borderThickness)
    {
        edges |= MaterialGraphResizeEdge_Top;
    }
    else if (mousePosition.y >= nodeRect.Max.y - borderThickness)
    {
        edges |= MaterialGraphResizeEdge_Bottom;
    }

    return edges;
}

ImGuiMouseCursor GetMaterialGraphResizeCursor(uint8_t edges)
{
    const bool horizontal = (edges & (MaterialGraphResizeEdge_Left | MaterialGraphResizeEdge_Right)) != 0;
    const bool vertical = (edges & (MaterialGraphResizeEdge_Top | MaterialGraphResizeEdge_Bottom)) != 0;
    if (horizontal && vertical)
    {
        const bool northwestToSoutheast =
            ((edges & MaterialGraphResizeEdge_Left) != 0 && (edges & MaterialGraphResizeEdge_Top) != 0) ||
            ((edges & MaterialGraphResizeEdge_Right) != 0 && (edges & MaterialGraphResizeEdge_Bottom) != 0);
        return northwestToSoutheast ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW;
    }
    if (horizontal)
    {
        return ImGuiMouseCursor_ResizeEW;
    }
    if (vertical)
    {
        return ImGuiMouseCursor_ResizeNS;
    }
    return ImGuiMouseCursor_Arrow;
}

bool CanPasteMaterialGraphNode(
    const MaterialShaderGraph& graph,
    const std::optional<MaterialShaderNode>& clipboardNode
)
{
    if (!clipboardNode.has_value())
    {
        return false;
    }
    if (clipboardNode->type == MaterialShaderNodeType::Output && MaterialGraphHasOutputNode(graph))
    {
        return false;
    }
    return true;
}

MaterialShaderNode* PasteMaterialGraphNode(
    MaterialShaderGraph& graph,
    const MaterialShaderNode& clipboardNode,
    const MaterialGraphNodePosition& position
)
{
    if (clipboardNode.type == MaterialShaderNodeType::Output && MaterialGraphHasOutputNode(graph))
    {
        return nullptr;
    }

    MaterialShaderNode newNode = clipboardNode;
    newNode.id = graph.nextNodeId++;
    newNode.position = position;
    if (!clipboardNode.name.empty())
    {
        newNode.name = clipboardNode.name + " Copy";
    }
    else
    {
        newNode.name = BuildMaterialGraphNodeName(clipboardNode.type, newNode.id);
    }
    graph.nodes.push_back(newNode);
    return &graph.nodes.back();
}

bool ApplyMaterialGraphNodeResize(
    MaterialShaderNode& node,
    uint8_t resizeEdges,
    const MaterialGraphNodePosition& startPosition,
    const ImVec2& startLogicalSize,
    const ImVec2& mouseDelta,
    float effectiveUiScale,
    float zoom
)
{
    if (effectiveUiScale <= 0.0f || zoom <= 0.0f || resizeEdges == MaterialGraphResizeEdge_None)
    {
        return false;
    }

    MaterialGraphNodePosition updatedPosition = startPosition;
    ImVec2 updatedSize = startLogicalSize;
    const ImVec2 minSize = GetMaterialGraphNodeMinSize(node.type);

    if ((resizeEdges & MaterialGraphResizeEdge_Left) != 0)
    {
        const float rightGraph = startPosition.x + startLogicalSize.x * effectiveUiScale;
        const float nextLeftGraph = startPosition.x + mouseDelta.x / zoom;
        updatedSize.x = std::max(minSize.x, (rightGraph - nextLeftGraph) / effectiveUiScale);
        updatedPosition.x = rightGraph - updatedSize.x * effectiveUiScale;
    }
    else if ((resizeEdges & MaterialGraphResizeEdge_Right) != 0)
    {
        updatedSize.x = std::max(minSize.x, startLogicalSize.x + mouseDelta.x / (effectiveUiScale * zoom));
    }

    if ((resizeEdges & MaterialGraphResizeEdge_Top) != 0)
    {
        const float bottomGraph = startPosition.y + startLogicalSize.y * effectiveUiScale;
        const float nextTopGraph = startPosition.y + mouseDelta.y / zoom;
        updatedSize.y = std::max(minSize.y, (bottomGraph - nextTopGraph) / effectiveUiScale);
        updatedPosition.y = bottomGraph - updatedSize.y * effectiveUiScale;
    }
    else if ((resizeEdges & MaterialGraphResizeEdge_Bottom) != 0)
    {
        updatedSize.y = std::max(minSize.y, startLogicalSize.y + mouseDelta.y / (effectiveUiScale * zoom));
    }

    const ImVec2 previousSize = GetMaterialGraphNodeLogicalSize(node);
    const bool positionChanged =
        std::abs(node.position.x - updatedPosition.x) > 0.01f || std::abs(node.position.y - updatedPosition.y) > 0.01f;
    const bool sizeChanged =
        std::abs(previousSize.x - updatedSize.x) > 0.01f || std::abs(previousSize.y - updatedSize.y) > 0.01f;
    if (!positionChanged && !sizeChanged)
    {
        return false;
    }

    node.position = updatedPosition;
    SetMaterialGraphNodeLogicalSize(node, updatedSize);
    return true;
}

ImRect BuildMaterialGraphNodeRect(
    const MaterialShaderNode& node,
    const ImVec2& canvasOrigin,
    const MaterialGraphNodePosition& viewOrigin,
    float uiScale,
    float zoom
)
{
    const ImVec2 nodePosition = ComputeNodeScreenPosition(node.position, canvasOrigin, viewOrigin, zoom);
    const ImVec2 nodeSize = GetMaterialGraphNodeSize(node, uiScale);
    return ImRect(nodePosition, ImVec2(nodePosition.x + nodeSize.x, nodePosition.y + nodeSize.y));
}

bool IsMouseOverMaterialGraphNode(
    const MaterialShaderGraph& graph,
    const ImVec2& mousePosition,
    const ImVec2& canvasOrigin,
    const MaterialGraphNodePosition& viewOrigin,
    float uiScale,
    float zoom
)
{
    return std::any_of(graph.nodes.begin(), graph.nodes.end(), [&](const MaterialShaderNode& node)
    {
        return BuildMaterialGraphNodeRect(node, canvasOrigin, viewOrigin, uiScale, zoom).Contains(mousePosition);
    });
}

std::string BuildMaterialGraphTextureImportSlotName(const MaterialShaderNode& node)
{
    return "graph_node_" + std::to_string(node.id) + "_texture";
}

bool DrawMaterialPbrControls(MaterialPbrSurfaceSettings& pbr)
{
    bool changed = false;
    changed |= ImGui::ColorEdit4("Base Factor", pbr.baseColorFactor);
    changed |= ImGui::SliderFloat("Metallic", &pbr.metallicFactor, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Roughness", &pbr.roughnessFactor, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Normal Scale", &pbr.normalScale, 0.0f, 4.0f, "%.2f");
    changed |= ImGui::SliderFloat("AO Strength", &pbr.occlusionStrength, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::ColorEdit3("Emissive", pbr.emissiveColor);
    changed |= ImGui::SliderFloat("Emissive Intensity", &pbr.emissiveIntensity, 0.0f, 8.0f, "%.2f");
    changed |= ImGui::SliderFloat("Opacity", &pbr.opacity, 0.0f, 1.0f, "%.2f");
    return changed;
}

MaterialGraphRenderedPin DrawMaterialGraphPinRow(
    const MaterialGraphPinDefinition& pinDefinition,
    uint32_t nodeId,
    bool input,
    float uiScale,
    bool linkDragActive,
    bool highlightCompatible,
    bool selected,
    uint32_t linkId,
    bool& pinPressed,
    bool& disconnectRequested
)
{
    const float pinRadius = 6.0f * uiScale;
    const float pinDiameter = pinRadius * 2.0f + 6.0f * uiScale;
    const ImU32 pinColor = GetMaterialGraphPinColor(pinDefinition.kind);

    MaterialGraphRenderedPin renderedPin{};
    renderedPin.nodeId = nodeId;
    renderedPin.slot = pinDefinition.slot;
    renderedPin.kind = pinDefinition.kind;
    renderedPin.input = input;
    renderedPin.linkId = linkId;

    ImGui::PushID(pinDefinition.slot);
    if (!input)
    {
        const float labelWidth = ImGui::CalcTextSize(pinDefinition.label).x;
        const float rowWidth = labelWidth + pinDiameter + 10.0f * uiScale;
        const float outputStartX = std::max(
            ImGui::GetCursorPosX(),
            ImGui::GetWindowContentRegionMax().x - rowWidth
        );
        ImGui::SetCursorPosX(outputStartX);
        ImGui::TextUnformatted(pinDefinition.label);
        ImGui::SameLine(0.0f, 8.0f * uiScale);
    }

    ImGui::InvisibleButton(input ? "InputPin" : "OutputPin", ImVec2(pinDiameter, pinDiameter));
    const bool hovered = ImGui::IsItemHovered();
    pinPressed = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    renderedPin.center = ImVec2(
        (ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
        (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f
    );

    ImU32 fillColor = pinColor;
    if (linkDragActive && highlightCompatible)
    {
        fillColor = IM_COL32(255, 214, 122, 255);
    }
    else if (selected)
    {
        fillColor = IM_COL32(255, 196, 64, 255);
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddCircleFilled(renderedPin.center, pinRadius, fillColor, 14);
    drawList->AddCircle(
        renderedPin.center,
        pinRadius + 1.25f * uiScale,
        hovered ? IM_COL32(255, 255, 255, 240) : IM_COL32(24, 28, 34, 230),
        14,
        1.5f * uiScale
    );

    if (input)
    {
        ImGui::SameLine(0.0f, 8.0f * uiScale);
        ImGui::TextUnformatted(pinDefinition.label);
        if (linkId != 0)
        {
            ImGui::SameLine(0.0f, 8.0f * uiScale);
            ImGui::TextDisabled("linked");
            ImGui::SameLine(0.0f, 6.0f * uiScale);
            disconnectRequested = ImGui::SmallButton("X");
        }
    }

    ImGui::PopID();
    return renderedPin;
}

MaterialGraphNodeDrawResult DrawMaterialGraphNode(
    ModelImportedMaterialInfo& material,
    MaterialShaderNode& node,
    const std::string& modelPath,
    uint32_t materialIndex,
    const ImVec2& canvasOrigin,
    const MaterialGraphNodePosition& viewOrigin,
    float uiScale,
    float zoom,
    bool nodeSelected,
    bool linkDragActive,
    bool nodeResizeActive,
    bool canPasteClipboardNode,
    uint32_t dragFromNodeId,
    std::string_view dragFromSlot,
    std::string* statusMessage
)
{
    MaterialGraphNodeDrawResult result{};
    const ImVec2 nodeSize = GetMaterialGraphNodeSize(node, uiScale);
    const ImVec4 headerColor = GetMaterialGraphHeaderColor(node.type);
    const bool allowDelete = node.type != MaterialShaderNodeType::Output;
    const float cornerRounding = 10.0f * uiScale;
    const float headerHeight = 34.0f * uiScale;
    const ImU32 headerFillColor = ImGui::ColorConvertFloat4ToU32(headerColor);
    const ImU32 nodeFillColor = nodeSelected ? IM_COL32(25, 30, 38, 248) : IM_COL32(18, 22, 29, 244);
    const ImU32 nodeBorderColor = nodeSelected ? kSelectionOutlineColor : IM_COL32(76, 90, 108, 224);
    const ImU32 titleColor = nodeSelected ? IM_COL32(252, 246, 228, 255) : IM_COL32(235, 241, 248, 255);
    const ImU32 badgeColor = nodeSelected ? IM_COL32(255, 236, 190, 235) : IM_COL32(242, 246, 252, 220);
    const ImVec2 nodePosition = ComputeNodeScreenPosition(node.position, canvasOrigin, viewOrigin, zoom);

    ImGui::SetCursorScreenPos(nodePosition);
    ImGui::PushID(static_cast<int>(node.id));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f * uiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * uiScale, 10.0f * uiScale));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild("Node", nodeSize, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetWindowFontScale(zoom);

    ImGuiWindow* nodeWindow = ImGui::GetCurrentWindow();
    const ImVec2 nodeMin = ImGui::GetWindowPos();
    const ImVec2 nodeMax(nodeMin.x + nodeSize.x, nodeMin.y + nodeSize.y);
    const ImRect nodeRect(nodeMin, nodeMax);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(nodeMin, nodeMax, nodeFillColor, cornerRounding);
    drawList->AddRectFilled(
        nodeMin,
        ImVec2(nodeMax.x, nodeMin.y + headerHeight),
        headerFillColor,
        cornerRounding
    );
    drawList->AddRect(
        nodeMin,
        nodeMax,
        nodeBorderColor,
        cornerRounding,
        0,
        nodeSelected ? 2.8f * uiScale : 1.4f * uiScale
    );

    const char* nodeTitle = node.name.empty() ? GetDefaultMaterialGraphNodeName(node.type) : node.name.c_str();
    const char* nodeTypeLabel = GetMaterialGraphNodeTypeLabel(node.type);
    const ImVec2 badgeMin(nodeMin.x + 10.0f * uiScale, nodeMin.y + 8.0f * uiScale);
    const ImVec2 badgeTextSize = ImGui::CalcTextSize(nodeTypeLabel);
    const ImVec2 badgeMax(
        badgeMin.x + badgeTextSize.x + 14.0f * uiScale,
        badgeMin.y + std::max(18.0f * uiScale, badgeTextSize.y + 6.0f * uiScale)
    );
    drawList->AddRectFilled(badgeMin, badgeMax, badgeColor, 7.0f * uiScale);
    drawList->AddText(
        ImVec2(badgeMin.x + 7.0f * uiScale, badgeMin.y + 3.0f * uiScale),
        IM_COL32(36, 40, 48, 255),
        nodeTypeLabel
    );
    drawList->AddText(
        ImVec2(badgeMax.x + 10.0f * uiScale, nodeMin.y + 9.0f * uiScale),
        titleColor,
        nodeTitle
    );

    if (allowDelete)
    {
        ImGui::SetCursorPos(ImVec2(nodeSize.x - 34.0f * uiScale, 7.0f * uiScale));
        if (ImGui::Button("X", ImVec2(24.0f * uiScale, 20.0f * uiScale)))
        {
            result.requestDelete = true;
        }
    }

    ImGui::SetCursorPos(ImVec2(12.0f * uiScale, headerHeight + 10.0f * uiScale));
    ImGui::TextDisabled("%s node", GetMaterialGraphNodeTypeLabel(node.type));

    const auto& inputPins = GetMaterialGraphInputPins(node.type);
    if (!inputPins.empty())
    {
        ImGui::SeparatorText("Inputs");
        for (const MaterialGraphPinDefinition& pinDefinition : inputPins)
        {
            const MaterialShaderLink* incomingLink =
                FindIncomingMaterialGraphLink(material.shaderGraph, node.id, pinDefinition.slot);
            const bool highlightCompatible =
                linkDragActive &&
                CanConnectMaterialGraphPins(
                    material.shaderGraph,
                    dragFromNodeId,
                    dragFromSlot,
                    node.id,
                    pinDefinition.slot
                );

            bool pinPressed = false;
            bool disconnectRequested = false;
            MaterialGraphRenderedPin renderedPin = DrawMaterialGraphPinRow(
                pinDefinition,
                node.id,
                true,
                uiScale,
                linkDragActive,
                highlightCompatible,
                incomingLink != nullptr,
                incomingLink != nullptr ? incomingLink->id : 0,
                pinPressed,
                disconnectRequested
            );
            if (pinPressed)
            {
                result.selected = true;
                if (linkDragActive)
                {
                    std::string failureReason;
                    if (CanConnectMaterialGraphPins(
                            material.shaderGraph,
                            dragFromNodeId,
                            dragFromSlot,
                            node.id,
                            pinDefinition.slot,
                            &failureReason
                        ))
                    {
                        result.connectedLinkId = ConnectMaterialGraphPins(
                            material.shaderGraph,
                            dragFromNodeId,
                            dragFromSlot,
                            node.id,
                            pinDefinition.slot
                        );
                        result.changed |= result.connectedLinkId != 0;
                    }
                    else if (statusMessage != nullptr && !failureReason.empty())
                    {
                        *statusMessage = failureReason;
                    }
                }
                else if (incomingLink != nullptr)
                {
                    result.selectedLinkId = incomingLink->id;
                }
            }
            if (disconnectRequested && incomingLink != nullptr)
            {
                RemoveMaterialGraphLink(material.shaderGraph, incomingLink->id);
                result.changed = true;
            }
            result.pins.push_back(std::move(renderedPin));
        }
    }

    switch (node.type)
    {
    case MaterialShaderNodeType::Texture:
    {
        ImGui::SeparatorText("Texture");
        ImGui::TextWrapped("This node outputs a texture sample for surface inputs or blend mask wiring.");
        const std::string idSuffix = "TextureNode_" + std::to_string(node.id);
        const std::string slotName = BuildMaterialGraphTextureImportSlotName(node);
        result.changed |= DrawGraphTextureSlotEditor(
            "Source",
            idSuffix.c_str(),
            modelPath,
            materialIndex,
            slotName.c_str(),
            node.texturePath,
            statusMessage
        );
        break;
    }
    case MaterialShaderNodeType::Scalar:
        ImGui::SeparatorText("Value");
        ImGui::TextWrapped("Use this node to drive blend factor or output scalar overrides.");
        result.changed |= ImGui::SliderFloat("Scalar", &node.scalarValue, 0.0f, 8.0f, "%.2f");
        break;
    case MaterialShaderNodeType::Color:
        ImGui::SeparatorText("Color");
        ImGui::TextWrapped("Feed this into base factor or emissive color inputs.");
        result.changed |= ImGui::ColorEdit4("Color", node.colorValue);
        break;
    case MaterialShaderNodeType::Surface:
        ImGui::SeparatorText("Surface");
        ImGui::TextWrapped("Connect texture nodes into the PBR texture slots, then route the surface output forward.");
        break;
    case MaterialShaderNodeType::Blend:
        ImGui::SeparatorText("Blend");
        ImGui::TextWrapped("This node mixes Surface A and Surface B. A connected scalar input overrides the default factor.");
        result.changed |= ImGui::SliderFloat("Default Factor", &node.scalarValue, 0.0f, 1.0f, "%.2f");
        break;
    case MaterialShaderNodeType::Output:
        ImGui::SeparatorText("PBR Defaults");
        ImGui::TextWrapped("These values are used as material defaults until matching scalar or color inputs are connected.");
        result.changed |= DrawMaterialPbrControls(node.pbr);
        break;
    default:
        break;
    }

    const auto& outputPins = GetMaterialGraphOutputPins(node.type);
    if (!outputPins.empty())
    {
        ImGui::SeparatorText("Outputs");
        for (const MaterialGraphPinDefinition& pinDefinition : outputPins)
        {
            bool pinPressed = false;
            bool disconnectRequested = false;
            MaterialGraphRenderedPin renderedPin = DrawMaterialGraphPinRow(
                pinDefinition,
                node.id,
                false,
                uiScale,
                linkDragActive,
                false,
                linkDragActive &&
                    dragFromNodeId == node.id &&
                    dragFromSlot == pinDefinition.slot,
                0,
                pinPressed,
                disconnectRequested
            );
            if (pinPressed)
            {
                result.requestStartLinkDrag = true;
                result.startLinkNodeId = node.id;
                result.startLinkSlot = pinDefinition.slot;
                result.selected = true;
            }
            result.pins.push_back(std::move(renderedPin));
        }
    }

    const bool nodeWidgetsHovered = ImGui::IsAnyItemHovered();
    const bool nodeControlsActive = GImGui->ActiveIdWindow == nodeWindow;
    const uint8_t hoveredResizeEdges =
        nodeSelected && !linkDragActive ? GetMaterialGraphResizeEdges(nodeRect, ImGui::GetIO().MousePos, uiScale) : 0;
    result.hovered = nodeRect.Contains(ImGui::GetIO().MousePos);
    result.capturesMouse = result.hovered || nodeControlsActive || nodeResizeActive;
    if (result.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        result.selected = true;
    }
    if (nodeSelected &&
        hoveredResizeEdges != MaterialGraphResizeEdge_None &&
        !nodeWidgetsHovered &&
        !nodeControlsActive &&
        !linkDragActive)
    {
        ImGui::SetMouseCursor(GetMaterialGraphResizeCursor(hoveredResizeEdges));
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            result.requestStartResize = true;
            result.resizeEdges = hoveredResizeEdges;
            result.selected = true;
        }
    }
    if (nodeSelected &&
        !nodeResizeActive &&
        hoveredResizeEdges == MaterialGraphResizeEdge_None &&
        result.hovered &&
        !nodeControlsActive &&
        !linkDragActive &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        node.position.x += ImGui::GetIO().MouseDelta.x / zoom;
        node.position.y += ImGui::GetIO().MouseDelta.y / zoom;
        result.changed = true;
        result.selected = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    result.min = ImGui::GetItemRectMin();
    result.max = ImGui::GetItemRectMax();

    if (ImGui::BeginPopupContextItem("NodeContextMenu"))
    {
        result.selected = true;
        if (ImGui::BeginMenu("Edit"))
        {
            if (ImGui::MenuItem("Copy Node", nullptr, false, allowDelete))
            {
                result.requestCopy = true;
            }
            if (ImGui::MenuItem("Paste Node", nullptr, false, canPasteClipboardNode))
            {
                result.requestPaste = true;
                result.pastePosition = MaterialGraphNodePosition{
                    node.position.x + 40.0f,
                    node.position.y + 40.0f
                };
            }
            if (ImGui::MenuItem("Delete Node", nullptr, false, allowDelete))
            {
                result.requestDelete = true;
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    ImGui::PopID();
    return result;
}

void DrawNodeConnection(
    ImDrawList* drawList,
    const ImVec2& from,
    const ImVec2& to,
    ImU32 color,
    float thickness
)
{
    if (drawList == nullptr)
    {
        return;
    }

    const float tangentOffset = std::max((to.x - from.x) * 0.5f, 70.0f);
    const ImVec2 control0(from.x + tangentOffset, from.y);
    const ImVec2 control1(to.x - tangentOffset, to.y);
    drawList->AddBezierCubic(from, control0, control1, to, color, thickness);
}

std::string ImportTextureIntoModelMaterialDirectory(
    const std::string& modelPath,
    uint32_t materialIndex,
    const char* slotName,
    const std::string& sourceTexturePath
)
{
    if (sourceTexturePath.empty())
    {
        return {};
    }

    const std::filesystem::path normalizedModelPath = NormalizeFilesystemPath(modelPath);
    const std::filesystem::path normalizedSourcePath = NormalizeFilesystemPath(sourceTexturePath);
    if (!std::filesystem::exists(normalizedModelPath))
    {
        throw std::runtime_error("Material target model does not exist: " + normalizedModelPath.string());
    }
    if (!std::filesystem::exists(normalizedSourcePath))
    {
        throw std::runtime_error("Selected texture does not exist: " + normalizedSourcePath.string());
    }

    const std::filesystem::path destinationDirectory =
        BuildImportedMaterialTextureDirectory(normalizedModelPath, materialIndex);
    std::filesystem::create_directories(destinationDirectory);

    const std::string normalizedSlotName =
        (slotName == nullptr || std::string(slotName).empty())
        ? "texture"
        : BuildImportedMaterialFilePrefix(normalizedModelPath, materialIndex) + "_" + std::string(slotName);
    const std::filesystem::path destinationPath =
        destinationDirectory / (normalizedSlotName + normalizedSourcePath.extension().string());
    if (NormalizeFilesystemPath(destinationPath) == normalizedSourcePath)
    {
        return destinationPath.string();
    }

    RemoveImportedTextureVariants(destinationDirectory, normalizedSlotName);
    std::filesystem::copy_file(
        normalizedSourcePath,
        destinationPath,
        std::filesystem::copy_options::overwrite_existing
    );
    return destinationPath.string();
}

bool IsSameOrDescendantPath(const std::filesystem::path& path, const std::filesystem::path& parent)
{
    const std::filesystem::path normalizedPath = path.lexically_normal();
    const std::filesystem::path normalizedParent = parent.lexically_normal();
    if (normalizedPath == normalizedParent)
    {
        return true;
    }

    std::error_code errorCode;
    const std::filesystem::path relativePath = std::filesystem::relative(normalizedPath, normalizedParent, errorCode);
    if (errorCode || relativePath.empty())
    {
        return false;
    }

    for (const std::filesystem::path& part : relativePath)
    {
        if (part == "..")
        {
            return false;
        }
    }

    return true;
}

bool ShouldDisplayAssetEntry(const std::filesystem::path& path)
{
    static const std::string kHiddenSidecarSuffix = ".miniengine_asset.yaml";
    const std::string fileName = ToLowerCopy(path.filename().string());
    return
        fileName.size() < kHiddenSidecarSuffix.size() ||
        fileName.compare(
            fileName.size() - kHiddenSidecarSuffix.size(),
            kHiddenSidecarSuffix.size(),
            kHiddenSidecarSuffix
        ) != 0;
}

std::string ComputeFileContentHash(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        return {};
    }

    uint64_t hash = 1469598103934665603ull;
    std::array<char, 8192> buffer{};
    while (input.good())
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize bytesRead = input.gcount();
        for (std::streamsize index = 0; index < bytesRead; ++index)
        {
            hash ^= static_cast<unsigned char>(buffer[static_cast<size_t>(index)]);
            hash *= 1099511628211ull;
        }
    }

    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << hash;
    return stream.str();
}

ImportedModelFingerprint ComputeImportedModelFingerprint(const std::filesystem::path& path)
{
    ImportedModelFingerprint fingerprint{};
    std::error_code errorCode;
    fingerprint.fileSize = std::filesystem::file_size(path, errorCode);
    if (errorCode)
    {
        fingerprint.fileSize = 0;
    }

    fingerprint.contentHash = ComputeFileContentHash(path);
    return fingerprint;
}

std::optional<ImportedModelFingerprint> ReadImportedModelFingerprintFromManifest(const std::filesystem::path& modelPath)
{
    const std::filesystem::path manifestPath = std::filesystem::path(modelPath.string() + ".miniengine_asset.yaml");
    if (!std::filesystem::exists(manifestPath))
    {
        return std::nullopt;
    }

    try
    {
        const YAML::Node root = YAML::LoadFile(manifestPath.string());
        const YAML::Node assetNode = root["asset"];
        const YAML::Node sourceNode = assetNode ? assetNode["source"] : YAML::Node{};
        if (!sourceNode || !sourceNode.IsMap())
        {
            return std::nullopt;
        }

        ImportedModelFingerprint fingerprint{};
        if (const YAML::Node value = sourceNode["file_size"]; value)
        {
            fingerprint.fileSize = value.as<uintmax_t>(0);
        }
        if (const YAML::Node value = sourceNode["content_hash"]; value)
        {
            fingerprint.contentHash = value.as<std::string>();
        }
        if (fingerprint.fileSize == 0 && fingerprint.contentHash.empty())
        {
            return std::nullopt;
        }

        return fingerprint;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

bool AreMatchingFingerprints(const ImportedModelFingerprint& left, const ImportedModelFingerprint& right)
{
    if (left.fileSize == 0 || right.fileSize == 0 || left.contentHash.empty() || right.contentHash.empty())
    {
        return false;
    }

    return left.fileSize == right.fileSize && left.contentHash == right.contentHash;
}

std::optional<DuplicateModelImportInfo> FindDuplicateImportedModel(
    const std::filesystem::path& sourceModelPath,
    const std::filesystem::path& assetRoot
)
{
    const std::filesystem::path normalizedSourcePath = NormalizeFilesystemPath(sourceModelPath);
    if (!std::filesystem::exists(normalizedSourcePath) || !IsSupportedModelAssetPath(normalizedSourcePath))
    {
        return std::nullopt;
    }

    const ImportedModelFingerprint sourceFingerprint = ComputeImportedModelFingerprint(normalizedSourcePath);
    if (sourceFingerprint.fileSize == 0 || sourceFingerprint.contentHash.empty())
    {
        return std::nullopt;
    }

    if (!std::filesystem::exists(assetRoot))
    {
        return std::nullopt;
    }

    std::error_code errorCode;
    for (std::filesystem::recursive_directory_iterator iterator(assetRoot, errorCode);
         !errorCode && iterator != std::filesystem::recursive_directory_iterator();
         iterator.increment(errorCode))
    {
        if (!iterator->is_regular_file(errorCode) || errorCode)
        {
            continue;
        }

        const std::filesystem::path candidatePath = NormalizeFilesystemPath(iterator->path());
        if (!IsSupportedModelAssetPath(candidatePath))
        {
            continue;
        }

        ImportedModelFingerprint candidateFingerprint =
            ReadImportedModelFingerprintFromManifest(candidatePath).value_or(ComputeImportedModelFingerprint(candidatePath));
        if (!AreMatchingFingerprints(sourceFingerprint, candidateFingerprint))
        {
            continue;
        }

        return DuplicateModelImportInfo{
            normalizedSourcePath.string(),
            candidatePath.string()
        };
    }

    return std::nullopt;
}

void CollectSortedDirectoryEntries(
    const std::filesystem::path& directory,
    std::vector<std::filesystem::directory_entry>& entries
)
{
    entries.clear();

    std::error_code errorCode;
    for (std::filesystem::directory_iterator iterator(directory, errorCode);
         !errorCode && iterator != std::filesystem::directory_iterator();
         iterator.increment(errorCode))
    {
        if (ShouldDisplayAssetEntry(iterator->path()))
        {
            entries.push_back(*iterator);
        }
    }

    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right)
    {
        std::error_code leftErrorCode;
        std::error_code rightErrorCode;
        const bool leftIsDirectory = left.is_directory(leftErrorCode) && !leftErrorCode;
        const bool rightIsDirectory = right.is_directory(rightErrorCode) && !rightErrorCode;
        if (leftIsDirectory != rightIsDirectory)
        {
            return leftIsDirectory > rightIsDirectory;
        }

        return ToLowerCopy(left.path().filename().string()) < ToLowerCopy(right.path().filename().string());
    });
}

void CollectSortedChildDirectories(
    const std::filesystem::path& directory,
    std::vector<std::filesystem::directory_entry>& directories
)
{
    directories.clear();

    std::vector<std::filesystem::directory_entry> entries;
    CollectSortedDirectoryEntries(directory, entries);
    directories.reserve(entries.size());
    for (const std::filesystem::directory_entry& entry : entries)
    {
        std::error_code errorCode;
        if (entry.is_directory(errorCode) && !errorCode)
        {
            directories.push_back(entry);
        }
    }
}

std::string BuildAssetBrowserPathLabel(const std::filesystem::path& assetRoot, const std::filesystem::path& path)
{
    const std::string rootLabel =
        assetRoot.filename().empty()
        ? assetRoot.string()
        : assetRoot.filename().string();

    std::error_code errorCode;
    const std::filesystem::path relativePath = std::filesystem::relative(path, assetRoot, errorCode);
    if (errorCode || relativePath.empty() || relativePath == ".")
    {
        return rootLabel;
    }

    std::string label = rootLabel;
    for (const std::filesystem::path& part : relativePath)
    {
        label += " / ";
        label += part.string();
    }

    return label;
}

bool DrawHorizontalSplitter(const char* id, float thickness, float height)
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.22f, 0.26f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.33f, 0.36f, 0.42f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.42f, 0.46f, 0.54f, 1.0f));
    const bool pressed = ImGui::Button(id, ImVec2(thickness, height));
    ImGui::PopStyleColor(3);

    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    return pressed || ImGui::IsItemActive();
}

void DrawAssetDirectoryTreeNode(
    const std::filesystem::path& directory,
    const std::filesystem::path& assetRoot,
    const std::string& currentBrowserDirectory,
    bool autoExpandCurrentPath,
    std::string& selectedAssetPath,
    bool& selectedAssetIsDirectory,
    std::string& browserDirectory
)
{
    std::vector<std::filesystem::directory_entry> childDirectories;
    CollectSortedChildDirectories(directory, childDirectories);

    const std::string normalizedDirectory = NormalizeAssetPath(directory);
    const bool isCurrentBrowserDirectory = normalizedDirectory == currentBrowserDirectory;
    const bool isSelected = selectedAssetPath == normalizedDirectory;
    const bool shouldOpen =
        autoExpandCurrentPath &&
        IsSameOrDescendantPath(std::filesystem::path(currentBrowserDirectory), directory);
    const bool hasChildren = !childDirectories.empty();

    if (shouldOpen)
    {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    }

    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow |
        ImGuiTreeNodeFlags_OpenOnDoubleClick |
        ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!hasChildren)
    {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    if (isSelected || isCurrentBrowserDirectory)
    {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    const std::string label = directory == assetRoot
        ? BuildAssetBrowserPathLabel(assetRoot, directory)
        : directory.filename().string();

    ImGui::PushID(normalizedDirectory.c_str());
    const bool isOpen = ImGui::TreeNodeEx("##AssetDirectoryTreeNode", flags, "%s", label.c_str());

    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool toggledOpen = ImGui::IsItemToggledOpen();

    if (clicked && !toggledOpen)
    {
        browserDirectory = normalizedDirectory;
        selectedAssetPath = normalizedDirectory;
        selectedAssetIsDirectory = true;
    }

    if (hovered)
    {
        ImGui::SetTooltip("%s", normalizedDirectory.c_str());
    }

    if (isOpen)
    {
        for (const std::filesystem::directory_entry& childDirectory : childDirectories)
        {
            DrawAssetDirectoryTreeNode(
                childDirectory.path(),
                assetRoot,
                currentBrowserDirectory,
                autoExpandCurrentPath,
                selectedAssetPath,
                selectedAssetIsDirectory,
                browserDirectory
            );
        }

        ImGui::TreePop();
    }
    ImGui::PopID();
}

std::string BuildAssetTypeLabel(const std::filesystem::path& path, bool isDirectory)
{
    if (isDirectory)
    {
        return "DIR";
    }

    const std::string extension = ToLowerCopy(path.extension().string());
    if (IsSupportedModelAssetPath(path))
    {
        return "GLTF";
    }
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga" || extension == ".dds")
    {
        return "TEX";
    }
    if (IsMaterialAssetPath(path))
    {
        return "MAT";
    }
    if (extension == ".yaml" || extension == ".yml")
    {
        return "DATA";
    }
    if (extension.empty())
    {
        return "FILE";
    }

    std::string label = extension.substr(1);
    std::transform(label.begin(), label.end(), label.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::toupper(character));
    });
    if (label.size() > 4)
    {
        label.resize(4);
    }
    return label;
}

std::filesystem::path MakeUniqueAssetFolderPath(const std::filesystem::path& parentDirectory, const std::string& baseName)
{
    const std::filesystem::path desiredPath = parentDirectory / baseName;
    if (!std::filesystem::exists(desiredPath))
    {
        return desiredPath;
    }

    for (uint32_t suffix = 1; suffix < 10000; ++suffix)
    {
        const std::filesystem::path candidate = parentDirectory / (baseName + " " + std::to_string(suffix));
        if (!std::filesystem::exists(candidate))
        {
            return candidate;
        }
    }

    throw std::runtime_error("Failed to create a unique folder name inside: " + parentDirectory.string());
}

std::string TrimCopy(const std::string& value)
{
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }

    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool ContainsPathSeparator(const std::string& value)
{
    return value.find('/') != std::string::npos || value.find('\\') != std::string::npos;
}

std::string RemapNormalizedPathAfterRename(
    const std::string& currentPath,
    const std::filesystem::path& oldPath,
    const std::filesystem::path& newPath
)
{
    if (currentPath.empty())
    {
        return currentPath;
    }

    const std::filesystem::path normalizedCurrent = NormalizeFilesystemPath(currentPath);
    if (!IsSameOrDescendantPath(normalizedCurrent, oldPath))
    {
        return currentPath;
    }

    std::error_code errorCode;
    const std::filesystem::path relativePath = std::filesystem::relative(normalizedCurrent, oldPath, errorCode);
    if (errorCode || relativePath == ".")
    {
        return NormalizeAssetPath(newPath);
    }

    return NormalizeAssetPath(newPath / relativePath);
}

void DrawAssetBrowserTile(
    const std::filesystem::directory_entry& entry,
    const std::filesystem::path& assetRoot,
    float effectiveUiScale,
    std::optional<std::string>& requestedModelPreviewPath,
    std::optional<std::string>& requestedMaterialPreviewPath,
    EditorUiFrameResult& result,
    std::string& copiedAssetPath,
    std::string& renameTargetPath,
    std::array<char, 256>& renameBuffer,
    std::string& renameError,
    bool& openRenamePopup,
    bool& focusRenameInput,
    std::string& pendingDeleteAssetPath,
    bool& openDeleteAssetPopup,
    std::string& browserDirectory,
    std::string& selectedAssetPath,
    bool& selectedAssetIsDirectory
)
{
    std::error_code errorCode;
    const std::filesystem::path path = entry.path();
    const std::string normalizedPath = NormalizeAssetPath(path);
    const bool isDirectory = entry.is_directory(errorCode) && !errorCode;
    const bool isSelected = selectedAssetPath == normalizedPath;
    const bool isSupportedModel = !isDirectory && IsSupportedModelAssetPath(path);
    const bool isMaterialAsset = !isDirectory && IsMaterialAssetPath(path);
    const std::string extension = ToLowerCopy(path.extension().string());
    const bool isSceneAsset = !isDirectory && !IsMaterialAssetPath(path) && (extension == ".yaml" || extension == ".yml");
    const std::string typeLabel = BuildAssetTypeLabel(path, isDirectory);
    const std::string displayName = path.filename().string();
    const std::string subtitle =
        isDirectory
        ? "Double-click to open"
        : (isSceneAsset
            ? "Double-click to open scene"
            : (isSupportedModel
                ? "Double-click to preview model"
                : (isMaterialAsset
                    ? "Double-click to preview material"
                    : BuildAssetBrowserPathLabel(assetRoot, path.parent_path()))));

    ImGui::TableNextColumn();
    ImGui::PushID(normalizedPath.c_str());
    const float tileWidth = std::max(ImGui::GetContentRegionAvail().x, 120.0f * effectiveUiScale);
    const float tileHeight = std::clamp(
        tileWidth * 0.68f,
        118.0f * effectiveUiScale,
        196.0f * effectiveUiScale
    );
    ImGui::InvisibleButton("AssetTile", ImVec2(tileWidth, tileHeight));

    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const ImRect tileRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    const ImU32 backgroundColor =
        isSelected ? kAssetTileSelectedColor : (hovered ? kAssetTileHoverColor : kAssetTileBackgroundColor);
    const ImU32 borderColor =
        isSelected ? kSelectionOutlineColor : (hovered ? kAssetTileHoverBorderColor : kAssetTileBorderColor);
    const ImU32 badgeColor =
        isDirectory
        ? IM_COL32(88, 117, 74, 255)
        : (IsSupportedModelAssetPath(path) ? IM_COL32(64, 102, 145, 255) : IM_COL32(88, 95, 108, 255));

    const float tilePadding = 10.0f * effectiveUiScale;
    const float tileRounding = 8.0f * effectiveUiScale;
    const float badgeHeight = 48.0f * effectiveUiScale;

    drawList->AddRectFilled(tileRect.Min, tileRect.Max, backgroundColor, tileRounding);
    drawList->AddRect(tileRect.Min, tileRect.Max, borderColor, tileRounding, 0, isSelected ? 2.0f : 1.0f);

    const ImRect badgeRect(
        ImVec2(tileRect.Min.x + tilePadding, tileRect.Min.y + tilePadding),
        ImVec2(tileRect.Max.x - tilePadding, tileRect.Min.y + tilePadding + badgeHeight)
    );
    drawList->AddRectFilled(badgeRect.Min, badgeRect.Max, badgeColor, 6.0f * effectiveUiScale);

    const ImVec2 typeTextSize = ImGui::CalcTextSize(typeLabel.c_str());
    drawList->AddText(
        ImVec2(
            badgeRect.Min.x + (badgeRect.GetWidth() - typeTextSize.x) * 0.5f,
            badgeRect.Min.y + (badgeRect.GetHeight() - typeTextSize.y) * 0.5f
        ),
        IM_COL32(255, 255, 255, 255),
        typeLabel.c_str()
    );

    const ImRect titleRect(
        ImVec2(tileRect.Min.x + tilePadding, badgeRect.Max.y + 10.0f * effectiveUiScale),
        ImVec2(tileRect.Max.x - tilePadding, tileRect.Max.y - 28.0f * effectiveUiScale)
    );
    ImGui::RenderTextEllipsis(drawList, titleRect.Min, titleRect.Max, titleRect.Max.x, displayName.c_str(), nullptr, nullptr);

    const ImRect subtitleRect(
        ImVec2(tileRect.Min.x + tilePadding, tileRect.Max.y - 20.0f * effectiveUiScale),
        ImVec2(tileRect.Max.x - tilePadding, tileRect.Max.y - tilePadding)
    );
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.76f, 0.83f, 1.0f));
    ImGui::RenderTextEllipsis(drawList, subtitleRect.Min, subtitleRect.Max, subtitleRect.Max.x, subtitle.c_str(), nullptr, nullptr);
    ImGui::PopStyleColor();

    if (clicked)
    {
        selectedAssetPath = normalizedPath;
        selectedAssetIsDirectory = isDirectory;
    }

    if (doubleClicked)
    {
        if (isDirectory)
        {
            browserDirectory = normalizedPath;
            selectedAssetPath = normalizedPath;
            selectedAssetIsDirectory = true;
        }
        else if (isSceneAsset)
        {
            result.actions.selectedSceneLoadPath = normalizedPath;
        }
        else if (isSupportedModel)
        {
            requestedModelPreviewPath = normalizedPath;
        }
        else if (isMaterialAsset)
        {
            requestedMaterialPreviewPath = normalizedPath;
        }
    }

    if (hovered)
    {
        ImGui::SetTooltip("%s", path.string().c_str());
    }

    if (ImGui::BeginPopupContextItem("AssetTileContext"))
    {
        selectedAssetPath = normalizedPath;
        selectedAssetIsDirectory = isDirectory;

        if (ImGui::MenuItem("New Folder"))
        {
            try
            {
                const std::filesystem::path targetDirectory =
                    isDirectory
                    ? path
                    : path.parent_path();
                const std::filesystem::path newFolderPath = MakeUniqueAssetFolderPath(targetDirectory, "New Folder");
                std::filesystem::create_directory(newFolderPath);
                browserDirectory = NormalizeAssetPath(targetDirectory);
                selectedAssetPath = NormalizeAssetPath(newFolderPath);
                selectedAssetIsDirectory = true;
                renameTargetPath = selectedAssetPath;
                renameError.clear();
                std::snprintf(renameBuffer.data(), renameBuffer.size(), "%s", newFolderPath.filename().string().c_str());
                openRenamePopup = true;
                focusRenameInput = true;
            }
            catch (const std::exception& error)
            {
                renameError = error.what();
            }
        }

        if (ImGui::MenuItem("Copy"))
        {
            copiedAssetPath = normalizedPath;
        }

        const bool canPaste = !copiedAssetPath.empty() && std::filesystem::exists(std::filesystem::path(copiedAssetPath));
        if (ImGui::MenuItem("Paste", nullptr, false, canPaste))
        {
            result.actions.pastedAsset = EditorUiActions::AssetPasteRequest{
                copiedAssetPath,
                isDirectory ? normalizedPath : NormalizeAssetPath(path.parent_path())
            };
            if (isDirectory)
            {
                browserDirectory = normalizedPath;
            }
        }

        if (ImGui::MenuItem("Delete"))
        {
            pendingDeleteAssetPath = normalizedPath;
            openDeleteAssetPopup = true;
        }

        if (ImGui::MenuItem("Rename"))
        {
            renameTargetPath = normalizedPath;
            renameError.clear();
            std::snprintf(renameBuffer.data(), renameBuffer.size(), "%s", path.filename().string().c_str());
            openRenamePopup = true;
            focusRenameInput = true;
        }

        ImGui::EndPopup();
    }

    if (isSupportedModel && ImGui::BeginDragDropSource())
    {
        const std::string payloadPath = normalizedPath;
        ImGui::SetDragDropPayload(
            kAssetModelDragDropPayloadId,
            payloadPath.c_str(),
            payloadPath.size() + 1
        );
        ImGui::TextUnformatted("Place Model In Scene");
        ImGui::TextWrapped("%s", displayName.c_str());
        ImGui::EndDragDropSource();
    }

    ImGui::PopID();
}

void DrawTopToolbar(
    bool& showCameraWindow,
    bool& showAssetManagerWindow,
    bool& showInputMonitorWindow,
    bool& showSceneWindow,
    bool& showThemeWindow,
    bool& showViewportWindow,
    float effectiveUiScale
)
{
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    if (mainViewport == nullptr)
    {
        return;
    }

    const float toolbarHeight = 44.0f * effectiveUiScale;
    ImGui::SetNextWindowPos(mainViewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(mainViewport->Size.x, toolbarHeight), ImGuiCond_Always);
    ImGui::SetNextWindowViewport(mainViewport->ID);
    const ImGuiWindowFlags toolbarFlags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings;

    if (!ImGui::Begin("Toolbar", nullptr, toolbarFlags))
    {
        ImGui::End();
        return;
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Panels");
    ImGui::SameLine();

    ImGui::BeginDisabled(showCameraWindow);
    if (ImGui::Button("Camera"))
    {
        showCameraWindow = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(showAssetManagerWindow);
    if (ImGui::Button("Asset Manager"))
    {
        showAssetManagerWindow = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(showInputMonitorWindow);
    if (ImGui::Button("Input Monitor"))
    {
        showInputMonitorWindow = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(showSceneWindow);
    if (ImGui::Button("Scene"))
    {
        showSceneWindow = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(showThemeWindow);
    if (ImGui::Button("Theme"))
    {
        showThemeWindow = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(showViewportWindow);
    if (ImGui::Button("Viewport"))
    {
        showViewportWindow = true;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Show All"))
    {
        showCameraWindow = true;
        showAssetManagerWindow = true;
        showInputMonitorWindow = true;
        showSceneWindow = true;
        showThemeWindow = true;
        showViewportWindow = true;
    }

    ImGui::SameLine();
    ImGui::TextDisabled("Close panels with the X button and reopen them here.");

    ImGui::End();
}

ImGuiID DrawDockspaceBelowToolbar(float toolbarHeight)
{
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    if (mainViewport == nullptr)
    {
        return 0;
    }

    const ImVec2 dockspacePosition(mainViewport->Pos.x, mainViewport->Pos.y + toolbarHeight);
    const ImVec2 dockspaceSize(
        mainViewport->Size.x,
        std::max(mainViewport->Size.y - toolbarHeight, 1.0f)
    );

    ImGui::SetNextWindowPos(dockspacePosition, ImGuiCond_Always);
    ImGui::SetNextWindowSize(dockspaceSize, ImGuiCond_Always);
    ImGui::SetNextWindowViewport(mainViewport->ID);

    const ImGuiWindowFlags dockspaceHostFlags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("EditorDockspaceHost", nullptr, dockspaceHostFlags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspaceId = ImGui::GetID("EditorDockspace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), kEditorDockspaceFlags);
    EnsureDefaultDockLayout(dockspaceId, dockspaceSize);
    ImGui::End();

    return dockspaceId;
}

bool ProjectWorldPointToViewport(
    const glm::vec3& worldPoint,
    const glm::mat4& viewProjection,
    const ViewportOverlayRect& viewportRect,
    ImVec2& projectedPoint
)
{
    const glm::vec4 clip = viewProjection * glm::vec4(worldPoint, 1.0f);
    if (clip.w <= 0.0f)
    {
        return false;
    }

    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    if (ndc.z < 0.0f || ndc.z > 1.0f)
    {
        return false;
    }

    projectedPoint = ImVec2(
        viewportRect.origin.x + (ndc.x * 0.5f + 0.5f) * viewportRect.size.x,
        viewportRect.origin.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * viewportRect.size.y
    );
    return true;
}

bool BuildProjectedSelectionBox(
    const IEditorWorld& scene,
    entt::entity entity,
    const ViewportMatrices& matrices,
    const ViewportOverlayRect& viewportRect,
    std::array<ImVec2, 8>& projectedCorners
)
{
    const ModelComponent& model = scene.GetModel(entity);
    if (!model.hasBounds || viewportRect.size.x <= 0.0f || viewportRect.size.y <= 0.0f)
    {
        return false;
    }

    const glm::mat4 modelMatrix = scene.GetModelMatrix(entity);
    const glm::mat4 viewProjection = matrices.projection * matrices.view;
    const auto localCorners = BuildBoundsCorners(model.minBounds, model.maxBounds);
    for (size_t index = 0; index < localCorners.size(); ++index)
    {
        const glm::vec3 worldPoint = glm::vec3(modelMatrix * glm::vec4(localCorners[index], 1.0f));
        if (!ProjectWorldPointToViewport(worldPoint, viewProjection, viewportRect, projectedCorners[index]))
        {
            return false;
        }
    }

    return true;
}

bool ComputeWorldBounds(
    const IEditorWorld& scene,
    entt::entity entity,
    glm::vec3& minBounds,
    glm::vec3& maxBounds
)
{
    const ModelComponent& model = scene.GetModel(entity);
    if (!model.hasBounds)
    {
        return false;
    }

    minBounds = glm::vec3(std::numeric_limits<float>::max());
    maxBounds = glm::vec3(std::numeric_limits<float>::lowest());
    const glm::mat4 modelMatrix = scene.GetModelMatrix(entity);
    for (const glm::vec3& corner : BuildBoundsCorners(model.minBounds, model.maxBounds))
    {
        const glm::vec3 worldPoint = glm::vec3(modelMatrix * glm::vec4(corner, 1.0f));
        minBounds = glm::min(minBounds, worldPoint);
        maxBounds = glm::max(maxBounds, worldPoint);
    }

    return true;
}

std::vector<ProjectedEntityCenter> ProjectSceneCenters(
    const IEditorWorld& scene,
    const ViewportMatrices& matrices,
    const ViewportOverlayRect& viewportRect
)
{
    std::vector<ProjectedEntityCenter> projectedCenters;
    if (viewportRect.size.x <= 0.0f || viewportRect.size.y <= 0.0f)
    {
        return projectedCenters;
    }

    const glm::mat4 viewProjection = matrices.projection * matrices.view;
    projectedCenters.reserve(scene.GetEntityOrder().size());

    scene.ForEachEntity([&](entt::entity entity, const TagComponent&, const TransformComponent&, const ModelComponent& model)
    {
        if (!model.hasBounds)
        {
            return;
        }

        ProjectedEntityCenter projected{};
        projected.entity = entity;
        std::array<ImVec2, 8> projectedCorners{};
        if (!BuildProjectedSelectionBox(scene, entity, matrices, viewportRect, projectedCorners))
        {
            return;
        }

        projected.min = projectedCorners.front();
        projected.max = projectedCorners.front();
        for (const ImVec2& corner : projectedCorners)
        {
            projected.min.x = std::min(projected.min.x, corner.x);
            projected.min.y = std::min(projected.min.y, corner.y);
            projected.max.x = std::max(projected.max.x, corner.x);
            projected.max.y = std::max(projected.max.y, corner.y);
        }

        const glm::vec3 localCenter = scene.GetBoundsCenter(entity);
        const glm::vec4 clip = viewProjection * scene.GetModelMatrix(entity) * glm::vec4(localCenter, 1.0f);
        if (clip.w <= 0.0f)
        {
            return;
        }

        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f || ndc.z < 0.0f || ndc.z > 1.0f)
        {
            return;
        }

        projected.center = ImVec2(
            viewportRect.origin.x + (ndc.x * 0.5f + 0.5f) * viewportRect.size.x,
            viewportRect.origin.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * viewportRect.size.y
        );
        projected.depth = ndc.z;
        projectedCenters.push_back(projected);
    });

    return projectedCenters;
}

entt::entity PickHoveredEntity(const std::vector<ProjectedEntityCenter>& projectedCenters)
{
    const ImVec2 mousePosition = ImGui::GetMousePos();
    entt::entity hoveredEntity = entt::null;
    float bestDepth = std::numeric_limits<float>::max();
    const float hitRadiusSquared = kSelectionCenterHitRadiusPixels * kSelectionCenterHitRadiusPixels;

    for (const ProjectedEntityCenter& projectedCenter : projectedCenters)
    {
        const bool insideBounds =
            mousePosition.x >= projectedCenter.min.x - kSelectionBoundsHitPaddingPixels &&
            mousePosition.x <= projectedCenter.max.x + kSelectionBoundsHitPaddingPixels &&
            mousePosition.y >= projectedCenter.min.y - kSelectionBoundsHitPaddingPixels &&
            mousePosition.y <= projectedCenter.max.y + kSelectionBoundsHitPaddingPixels;
        const float dx = mousePosition.x - projectedCenter.center.x;
        const float dy = mousePosition.y - projectedCenter.center.y;
        const float distanceSquared = dx * dx + dy * dy;
        const bool insideCenterRadius = distanceSquared <= hitRadiusSquared;
        if ((!insideBounds && !insideCenterRadius) || projectedCenter.depth >= bestDepth)
        {
            continue;
        }

        bestDepth = projectedCenter.depth;
        hoveredEntity = projectedCenter.entity;
    }

    return hoveredEntity;
}

void DrawViewportSelectionOverlay(
    const IEditorWorld& scene,
    const ViewportMatrices& matrices,
    const ViewportOverlayRect& viewportRect,
    const std::vector<ProjectedEntityCenter>& projectedCenters
)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (drawList == nullptr)
    {
        return;
    }

    if (!scene.HasSelection())
    {
        return;
    }

    std::array<ImVec2, 8> projectedSelectionCorners{};
    if (!BuildProjectedSelectionBox(scene, scene.GetSelectedEntity(), matrices, viewportRect, projectedSelectionCorners))
    {
        return;
    }

    for (const auto& edge : kBoundsEdges)
    {
        drawList->AddLine(
            projectedSelectionCorners[edge.first],
            projectedSelectionCorners[edge.second],
            kSelectionOutlineColor,
            kSelectionOutlineThickness
        );
    }

    for (const ImVec2& corner : projectedSelectionCorners)
    {
        drawList->AddCircleFilled(corner, 2.5f, kSelectionOutlineColor);
    }
}

bool DrawOperationButton(const char* label, ImGuizmo::OPERATION value, ImGuizmo::OPERATION& current)
{
    const bool selected = current == value;
    if (ImGui::RadioButton(label, selected))
    {
        current = value;
        return true;
    }

    return false;
}

void DrawTransformComponent(TransformComponent& transform)
{
    ImGui::DragFloat3(
        "Translation (m)",
        glm::value_ptr(transform.translation),
        0.05f,
        -WorldUnits::kUiTransformTranslationRangeMeters,
        WorldUnits::kUiTransformTranslationRangeMeters,
        "%.3f"
    );
    ImGui::DragFloat3("Rotation", glm::value_ptr(transform.rotationDegrees), 0.5f);
    ImGui::DragFloat3(
        "Scale (1 = source meters)",
        glm::value_ptr(transform.scale),
        0.02f,
        WorldUnits::kMinimumScale,
        WorldUnits::kUiTransformScaleMax,
        "%.3f"
    );
    transform.scale = glm::max(transform.scale, WorldUnits::kMinimumScale3);
}

void DrawGizmoControls(GizmoSettings& gizmo)
{
    DrawOperationButton("Translate", ImGuizmo::TRANSLATE, gizmo.operation);
    ImGui::SameLine();
    DrawOperationButton("Rotate", ImGuizmo::ROTATE, gizmo.operation);
    ImGui::SameLine();
    DrawOperationButton("Scale", ImGuizmo::SCALE, gizmo.operation);

    const bool worldMode = gizmo.mode == ImGuizmo::WORLD;
    if (ImGui::RadioButton("World", worldMode))
    {
        gizmo.mode = ImGuizmo::WORLD;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Local", !worldMode))
    {
        gizmo.mode = ImGuizmo::LOCAL;
    }

    ImGui::Checkbox("Use Snap", &gizmo.useSnap);
    ImGui::DragFloat3(
        "Move Snap (m)",
        glm::value_ptr(gizmo.translationSnap),
        0.05f,
        WorldUnits::kUiCameraNearMinMeters,
        WorldUnits::kUiTranslationSnapMaxMeters,
        "%.2f"
    );
    ImGui::DragFloat("Rotate Snap", &gizmo.rotationSnap, 0.5f, 1.0f, 90.0f, "%.1f deg");
    ImGui::DragFloat3("Scale Snap", glm::value_ptr(gizmo.scaleSnap), 0.01f, 0.01f, WorldUnits::kUiScaleSnapMax, "%.2f");
}

void RefreshViewportMatrices(
    Camera& camera,
    ViewportMatrices& matrices,
    const IEditorWorld& scene,
    RenderExtent viewportExtent,
    RenderBackendType currentBackendType
)
{
    const bool useZeroToOneDepth = UsesZeroToOneDepth(currentBackendType);
    const bool invertRenderYAxis = UsesInvertedRenderYAxis(currentBackendType);
    matrices.view = camera.GetViewMatrix();
    matrices.projection = camera.GetProjectionMatrix(viewportExtent, false, useZeroToOneDepth);
    matrices.renderProjection = camera.GetProjectionMatrix(viewportExtent, invertRenderYAxis, useZeroToOneDepth);
    matrices.model =
        scene.HasSelection() ? scene.GetModelMatrix(scene.GetSelectedEntity()) : glm::mat4(1.0f);
}

glm::vec3 ComputeViewportDropPosition(
    const Camera& camera,
    const ViewportOverlayRect& viewportRect,
    RenderExtent viewportExtent,
    RenderBackendType currentBackendType,
    const ImVec2& mousePosition
)
{
    const float normalizedX = glm::clamp(
        (mousePosition.x - viewportRect.origin.x) / std::max(viewportRect.size.x, 1.0f),
        0.0f,
        1.0f
    );
    const float normalizedY = glm::clamp(
        (mousePosition.y - viewportRect.origin.y) / std::max(viewportRect.size.y, 1.0f),
        0.0f,
        1.0f
    );

    const float ndcX = normalizedX * 2.0f - 1.0f;
    const float ndcY = 1.0f - normalizedY * 2.0f;
    const bool useZeroToOneDepth = UsesZeroToOneDepth(currentBackendType);
    const glm::mat4 view = camera.GetViewMatrix();
    const glm::mat4 projection = camera.GetProjectionMatrix(viewportExtent, false, useZeroToOneDepth);
    const glm::mat4 inverseViewProjection = glm::inverse(projection * view);

    auto unproject = [&](float clipZ) -> glm::vec3
    {
        const glm::vec4 clipPoint(ndcX, ndcY, clipZ, 1.0f);
        const glm::vec4 worldPoint = inverseViewProjection * clipPoint;
        if (std::abs(worldPoint.w) <= 0.0001f)
        {
            return camera.position;
        }

        return glm::vec3(worldPoint) / worldPoint.w;
    };

    const glm::vec3 nearPoint = unproject(useZeroToOneDepth ? 0.0f : -1.0f);
    const glm::vec3 farPoint = unproject(1.0f);
    glm::vec3 rayDirection = farPoint - nearPoint;
    if (glm::length(rayDirection) <= 0.0001f)
    {
        return camera.position + camera.GetForward() * 4.0f;
    }

    rayDirection = glm::normalize(rayDirection);
    const float denominator = rayDirection.y;
    if (std::abs(denominator) > 0.0001f)
    {
        const float intersectionDistance = -camera.position.y / denominator;
        if (intersectionDistance > 0.0f)
        {
            return camera.position + rayDirection * intersectionDistance;
        }
    }

    return camera.position + rayDirection * 4.0f;
}

void HandleViewportAssetDropTarget(
    EditorUiFrameResult& result,
    const Camera& camera,
    const ViewportOverlayRect& viewportRect,
    RenderExtent viewportExtent,
    RenderBackendType currentBackendType
)
{
    if (!ImGui::BeginDragDropTarget())
    {
        return;
    }

    const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
        kAssetModelDragDropPayloadId,
        ImGuiDragDropFlags_AcceptBeforeDelivery
    );
    if (payload != nullptr)
    {
        const std::string payloadPath = ReadDragDropPayloadString(*payload);
        const bool isSupportedModel = IsSupportedModelAssetPath(std::filesystem::path(payloadPath));
        std::optional<EditorUiActions::ViewportModelPlacement> hoveredPlacement;
        if (isSupportedModel)
        {
            hoveredPlacement = EditorUiActions::ViewportModelPlacement{
                payloadPath,
                ComputeViewportDropPosition(
                    camera,
                    viewportRect,
                    viewportExtent,
                    currentBackendType,
                    ImGui::GetMousePos()
                )
            };
            result.actions.hoveredViewportModel = hoveredPlacement;
        }

        const ImVec2 max(viewportRect.origin.x + viewportRect.size.x, viewportRect.origin.y + viewportRect.size.y);
        if (viewportRect.drawList != nullptr)
        {
            viewportRect.drawList->AddRectFilled(viewportRect.origin, max, kViewportDropOverlayColor, 0.0f);
            viewportRect.drawList->AddRect(viewportRect.origin, max, kViewportDropOutlineColor, 0.0f, 0, 2.0f);
            const ImVec2 textOrigin(viewportRect.origin.x + 18.0f, viewportRect.origin.y + 18.0f);
            viewportRect.drawList->AddText(textOrigin, IM_COL32(245, 250, 245, 255), "Drag model into scene");
            if (hoveredPlacement.has_value())
            {
                char placementLabel[192]{};
                std::snprintf(
                    placementLabel,
                    sizeof(placementLabel),
                    "Release to place at %.2f, %.2f, %.2f",
                    hoveredPlacement->worldPosition.x,
                    hoveredPlacement->worldPosition.y,
                    hoveredPlacement->worldPosition.z
                );
                viewportRect.drawList->AddText(
                    ImVec2(textOrigin.x, textOrigin.y + 22.0f),
                    IM_COL32(228, 244, 228, 255),
                    placementLabel
                );
            }
        }

        if (payload->IsDelivery() && hoveredPlacement.has_value())
        {
            result.actions.droppedViewportModel = *hoveredPlacement;
        }
    }

    ImGui::EndDragDropTarget();
}

void HandleViewportShortcuts(IEditorWorld& scene, Camera& camera, const ViewportOverlayRect& viewportRect)
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard || !scene.HasSelection() || !viewportRect.focused)
    {
        return;
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right))
    {
        return;
    }

    GizmoSettings& gizmo = scene.GetGizmoSettings();
    if (ImGui::IsKeyPressed(ImGuiKey_W, false))
    {
        gizmo.operation = ImGuizmo::TRANSLATE;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_E, false))
    {
        gizmo.operation = ImGuizmo::ROTATE;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_R, false))
    {
        gizmo.operation = ImGuizmo::SCALE;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false))
    {
        glm::vec3 minBounds{};
        glm::vec3 maxBounds{};
        if (ComputeWorldBounds(scene, scene.GetSelectedEntity(), minBounds, maxBounds))
        {
            camera.FrameBounds(minBounds, maxBounds);
        }
    }
}

void HandleViewportSelection(
    IEditorWorld& scene,
    const std::vector<ProjectedEntityCenter>& projectedCenters,
    const ViewportOverlayRect& viewportRect
)
{
    if (!viewportRect.hovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        return;
    }

    if (scene.HasSelection() && ImGuizmo::IsUsing())
    {
        return;
    }

    scene.SetSelectedEntity(PickHoveredEntity(projectedCenters));
}

void DrawViewManipulator(Camera& camera, ViewportMatrices& matrices, const ViewportOverlayRect& viewportRect)
{
    if (viewportRect.size.x <= 0.0f || viewportRect.size.y <= 0.0f || viewportRect.drawList == nullptr)
    {
        return;
    }

    ImGuizmo::SetDrawlist(viewportRect.drawList);
    ImGuizmo::ViewManipulate(
        glm::value_ptr(matrices.view),
        7.5f,
        ImVec2(viewportRect.origin.x + viewportRect.size.x - 144.0f, viewportRect.origin.y + 16.0f),
        ImVec2(128.0f, 128.0f),
        IM_COL32(32, 32, 32, 180)
    );
    camera.SetFromViewMatrix(matrices.view);
    matrices.view = camera.GetViewMatrix();
}

void DrawGizmoOverlay(IEditorWorld& scene, ViewportMatrices& matrices, const ViewportOverlayRect& viewportRect)
{
    if (!scene.HasSelection() || viewportRect.size.x <= 0.0f || viewportRect.size.y <= 0.0f || viewportRect.drawList == nullptr)
    {
        return;
    }

    entt::entity selectedEntity = scene.GetSelectedEntity();
    GizmoSettings& gizmo = scene.GetGizmoSettings();
    matrices.model = scene.GetModelMatrix(selectedEntity);
    const glm::vec3 localCenter = scene.GetBoundsCenter(selectedEntity);
    const glm::mat4 pivotOffset = glm::translate(glm::mat4(1.0f), localCenter);
    const glm::mat4 inversePivotOffset = glm::translate(glm::mat4(1.0f), -localCenter);
    glm::mat4 gizmoMatrix = matrices.model * pivotOffset;

    std::array<float, 3> snapValues = {
        gizmo.translationSnap.x,
        gizmo.translationSnap.y,
        gizmo.translationSnap.z
    };
    if (gizmo.operation == ImGuizmo::ROTATE)
    {
        snapValues = { gizmo.rotationSnap, 0.0f, 0.0f };
    }
    else if (gizmo.operation == ImGuizmo::SCALE)
    {
        snapValues = { gizmo.scaleSnap.x, gizmo.scaleSnap.y, gizmo.scaleSnap.z };
    }

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetID(static_cast<int>(entt::to_integral(selectedEntity)));
    ImGuizmo::SetDrawlist(viewportRect.drawList);
    ImGuizmo::SetRect(viewportRect.origin.x, viewportRect.origin.y, viewportRect.size.x, viewportRect.size.y);
    ImGuizmo::Manipulate(
        glm::value_ptr(matrices.view),
        glm::value_ptr(matrices.projection),
        gizmo.operation,
        gizmo.mode,
        glm::value_ptr(gizmoMatrix),
        nullptr,
        gizmo.useSnap ? snapValues.data() : nullptr
    );

    if (!ImGuizmo::IsUsing())
    {
        return;
    }

    matrices.model = gizmoMatrix * inversePivotOffset;
    scene.ApplyTransformMatrix(selectedEntity, matrices.model);
}

}

void EditorUiController::BeginFrame(SDL_Window* window, const EngineSettings& settings)
{
    m_window = window;
    if (!m_hasCapturedBaseStyle)
    {
        m_baseStyle = ImGui::GetStyle();
        m_hasCapturedBaseStyle = true;
    }
    if (!m_hasCapturedDefaultThemeColors)
    {
        CaptureDefaultThemeColors();
        m_hasCapturedDefaultThemeColors = true;
    }
    if (!m_hasAppliedEngineSettings)
    {
        ApplyEngineSettings(settings);
        m_hasAppliedEngineSettings = true;
    }

    ApplyUiScale();
    ImGuizmo::BeginFrame();
}

void EditorUiController::OpenModelProcessorWindow(const std::string& modelPath)
{
    const std::filesystem::path normalizedPath = NormalizeFilesystemPath(modelPath);
    ResetMaterialShadedPreviewCache("ModelDraftPreviewCanvas");

    m_showModelProcessorWindow = true;
    m_modelProcessorModelPath = normalizedPath.string();
    m_modelProcessorDisplayName = normalizedPath.filename().string();
    m_modelProcessorStatusMessage.clear();
    m_modelProcessorSelectedMaterialIndex = 0;
    m_modelProcessorSelectedUvSubmeshIndex = 0;
    m_modelProcessorDirty = false;
    m_modelProcessorLoadedModel = LoadedModelData{};
    m_modelPreviewYaw = 0.55f;
    m_modelPreviewPitch = 0.35f;
    m_modelPreviewDistance = 3.0f;
    m_modelPreviewAutoFramePending = true;
    m_materialGraphSelectedNodeId = 0;
    m_materialGraphSelectedLinkId = 0;
    m_materialGraphResizeNodeId = 0;
    m_materialGraphLinkDragActive = false;
    m_materialGraphNodeResizeActive = false;
    m_materialGraphLinkDragFromNodeId = 0;
    m_materialGraphLinkDragFromSlot.clear();
    m_materialGraphViewOrigin = MaterialGraphNodePosition{};
    m_materialGraphResizeStartPosition = MaterialGraphNodePosition{};
    m_materialGraphZoom = 1.0f;
    m_materialGraphResizeStartMouse = ImVec2(0.0f, 0.0f);
    m_materialGraphResizeStartSize = ImVec2(0.0f, 0.0f);
    m_materialGraphPanningActive = false;
    m_materialGraphResizeEdges = 0;
    m_openMaterialGraphAddNodePopup = false;
    m_modelProcessorMaterials.clear();
    m_modelProcessorMaterialAssetPaths.clear();

    try
    {
        const LoadedModelData loadedModel = ModelLoader::LoadModel(m_modelProcessorModelPath);
        m_modelProcessorLoadedModel = loadedModel;
        m_modelProcessorMaterials =
            LoadEffectiveImportedModelMaterials(normalizedPath, loadedModel, &m_modelProcessorMaterialAssetPaths);
    }
    catch (const std::exception& error)
    {
        m_modelProcessorStatusMessage = error.what();
    }
}

void EditorUiController::BeginAssetRename(const std::string& assetPath)
{
    const std::filesystem::path normalizedPath = NormalizeFilesystemPath(assetPath);
    m_assetRenameTargetPath = normalizedPath.string();
    m_assetRenameError.clear();
    m_focusAssetRenameInput = true;
    std::snprintf(
        m_assetRenameBuffer.data(),
        m_assetRenameBuffer.size(),
        "%s",
        normalizedPath.filename().string().c_str()
    );
    m_openAssetRenamePopup = true;
}

bool EditorUiController::CommitAssetRename(const std::filesystem::path& assetRoot)
{
    const std::filesystem::path sourcePath = NormalizeFilesystemPath(m_assetRenameTargetPath);
    const std::string requestedName = TrimCopy(m_assetRenameBuffer.data());
    if (requestedName.empty())
    {
        m_assetRenameError = "Name cannot be empty.";
        return false;
    }
    if (requestedName == "." || requestedName == ".." || ContainsPathSeparator(requestedName))
    {
        m_assetRenameError = "Name cannot contain path separators.";
        return false;
    }
    if (!std::filesystem::exists(sourcePath))
    {
        m_assetRenameError = "The asset no longer exists.";
        return false;
    }

    const std::filesystem::path targetPath = sourcePath.parent_path() / requestedName;
    if (NormalizeFilesystemPath(targetPath) == sourcePath)
    {
        m_openAssetRenamePopup = false;
        m_assetRenameError.clear();
        return true;
    }
    if (!IsSameOrDescendantPath(targetPath.parent_path(), assetRoot))
    {
        m_assetRenameError = "Rename target is outside the assets directory.";
        return false;
    }
    if (std::filesystem::exists(targetPath))
    {
        m_assetRenameError = "An asset with that name already exists.";
        return false;
    }

    std::filesystem::rename(sourcePath, targetPath);

    m_selectedAssetPath = RemapNormalizedPathAfterRename(m_selectedAssetPath, sourcePath, targetPath);
    m_assetBrowserDirectory = RemapNormalizedPathAfterRename(m_assetBrowserDirectory, sourcePath, targetPath);
    m_copiedAssetPath = RemapNormalizedPathAfterRename(m_copiedAssetPath, sourcePath, targetPath);
    m_assetRenameTargetPath.clear();
    m_assetRenameError.clear();
    m_openAssetRenamePopup = false;
    return true;
}

void EditorUiController::RequestAssetDelete(const std::string& assetPath)
{
    m_pendingDeleteAssetPath = NormalizeFilesystemPath(assetPath).string();
    m_openDeleteAssetPopup = true;
}

void EditorUiController::ConfirmRequestedAssetDelete(
    const std::filesystem::path& assetRoot,
    const std::string& normalizedAssetRoot,
    EditorUiFrameResult& result
)
{
    if (m_pendingDeleteAssetPath.empty())
    {
        return;
    }

    result.actions.deleteAssetPath = m_pendingDeleteAssetPath;
    const std::filesystem::path selectedAssetPath(m_pendingDeleteAssetPath);
    if (IsSameOrDescendantPath(m_assetBrowserDirectory, selectedAssetPath))
    {
        const std::filesystem::path fallbackDirectory = selectedAssetPath.parent_path().empty()
            ? assetRoot
            : selectedAssetPath.parent_path();
        m_assetBrowserDirectory = IsSameOrDescendantPath(fallbackDirectory, assetRoot)
            ? NormalizeAssetPath(fallbackDirectory)
            : normalizedAssetRoot;
    }
    if (m_selectedAssetPath == m_pendingDeleteAssetPath)
    {
        m_selectedAssetPath.clear();
        m_selectedAssetIsDirectory = false;
    }
    if (m_copiedAssetPath == m_pendingDeleteAssetPath)
    {
        m_copiedAssetPath.clear();
    }
    m_pendingDeleteAssetPath.clear();
    m_openDeleteAssetPopup = false;
}

void EditorUiController::CloseModelProcessorWindow()
{
    ResetMaterialShadedPreviewCache("ModelDraftPreviewCanvas");
    m_showModelProcessorWindow = false;
    m_modelProcessorModelPath.clear();
    m_modelProcessorDisplayName.clear();
    m_modelProcessorStatusMessage.clear();
    m_modelProcessorMaterials.clear();
    m_modelProcessorMaterialAssetPaths.clear();
    m_modelProcessorSelectedMaterialIndex = 0;
    m_modelProcessorDirty = false;
    m_materialGraphSelectedNodeId = 0;
    m_materialGraphSelectedLinkId = 0;
    m_materialGraphResizeNodeId = 0;
    m_materialGraphLinkDragActive = false;
    m_materialGraphNodeResizeActive = false;
    m_materialGraphLinkDragFromNodeId = 0;
    m_materialGraphLinkDragFromSlot.clear();
    m_materialGraphViewOrigin = MaterialGraphNodePosition{};
    m_materialGraphResizeStartPosition = MaterialGraphNodePosition{};
    m_materialGraphZoom = 1.0f;
    m_materialGraphResizeStartMouse = ImVec2(0.0f, 0.0f);
    m_materialGraphResizeStartSize = ImVec2(0.0f, 0.0f);
    m_materialGraphPanningActive = false;
    m_materialGraphResizeEdges = 0;
    m_openMaterialGraphAddNodePopup = false;
}

void EditorUiController::OpenMaterialPreviewWindow(const std::string& materialPath)
{
    const std::filesystem::path normalizedPath = NormalizeFilesystemPath(materialPath);

    m_showMaterialPreviewWindow = true;
    m_materialPreviewAssetPath = normalizedPath.string();
    m_materialPreviewDisplayName = normalizedPath.filename().string();
    LoadMaterialPreviewAsset(
        normalizedPath,
        m_materialPreviewMaterial,
        m_materialPreviewModelPath,
        m_materialPreviewMaterialIndex,
        m_materialPreviewStatusMessage
    );
}

void EditorUiController::CloseMaterialPreviewWindow()
{
    m_showMaterialPreviewWindow = false;
    m_materialPreviewAssetPath.clear();
    m_materialPreviewDisplayName.clear();
    m_materialPreviewModelPath.clear();
    m_materialPreviewStatusMessage.clear();
    m_materialPreviewMaterial = ModelImportedMaterialInfo{};
    m_materialPreviewMaterialIndex = 0;
}

EditorUiFrameResult EditorUiController::Draw(
    Camera& camera,
    ViewportMatrices& matrices,
    IEditorWorld& scene,
    const std::string& currentModelPath,
    const std::string& lastLoadError,
    const std::string& lastSceneIoError,
    ImTextureID viewportTextureId,
    RenderExtent viewportExtent,
    RenderBackendType currentBackendType
)
{
    EditorUiFrameResult result{};
    result.viewportExtent = viewportExtent;
    const float previousUiScale = m_uiScale;
    const bool previousShowCameraWindow = m_showCameraWindow;
    const bool previousShowAssetManagerWindow = m_showAssetManagerWindow;
    const bool previousShowInputMonitorWindow = m_showInputMonitorWindow;
    const bool previousShowSceneWindow = m_showSceneWindow;
    const bool previousShowThemeWindow = m_showThemeWindow;
    const bool previousShowViewportWindow = m_showViewportWindow;
    std::optional<std::string> requestedModelPreviewPath;
    std::optional<std::string> requestedMaterialPreviewPath;

    if (m_showModelProcessorWindow)
    {
        std::error_code processorErrorCode;
        const std::filesystem::path processorModelPath(m_modelProcessorModelPath);
        if (m_modelProcessorModelPath.empty() ||
            !std::filesystem::exists(processorModelPath, processorErrorCode) ||
            processorErrorCode ||
            !IsSupportedModelAssetPath(processorModelPath))
        {
            CloseModelProcessorWindow();
        }
    }
    if (m_showMaterialPreviewWindow)
    {
        std::error_code materialErrorCode;
        const std::filesystem::path materialPreviewPath(m_materialPreviewAssetPath);
        if (m_materialPreviewAssetPath.empty() ||
            !std::filesystem::exists(materialPreviewPath, materialErrorCode) ||
            materialErrorCode ||
            !IsMaterialAssetPath(materialPreviewPath))
        {
            CloseMaterialPreviewWindow();
        }
    }

    const float toolbarHeight = 44.0f * m_effectiveUiScale;
    DrawDockspaceBelowToolbar(toolbarHeight);

    DrawTopToolbar(
        m_showCameraWindow,
        m_showAssetManagerWindow,
        m_showInputMonitorWindow,
        m_showSceneWindow,
        m_showThemeWindow,
        m_showViewportWindow,
        m_effectiveUiScale
    );

    if (m_showCameraWindow)
    {
        if (ImGui::Begin("Camera", &m_showCameraWindow))
        {
            if (ImGui::SliderFloat("UI Scale Multiplier", &m_uiScale, 0.75f, 2.50f, "%.2f x"))
            {
                ApplyUiScale();
            }
            const ImGuiIO& io = ImGui::GetIO();
            ImGui::Text("Platform UI Profile: %s", platform::ui::GetCurrentOperatingSystemName());
            ImGui::Text("Window DPI Scale: %.2f x", platform::ui::ResolveWindowUiScale(m_window));
            ImGui::Text("Effective UI Scale: %.2f x", m_effectiveUiScale);
            ImGui::Text("Framebuffer Scale: %.2f x %.2f", io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            ImGui::Text("World Units: 1.0 = %.1f meter", WorldUnits::kMetersPerUnit);
            ImGui::Text("Move: WASD");
            ImGui::Text("Look: Hold Right Mouse");
            ImGui::Text("Pan: Hold Middle Mouse");
            ImGui::SliderFloat3(
                "Position (m)",
                &camera.position.x,
                -WorldUnits::kUiCameraPositionRangeMeters,
                WorldUnits::kUiCameraPositionRangeMeters,
                "%.2f"
            );
            ImGui::SliderFloat("Yaw", &camera.yawDegrees, -180.0f, 180.0f);
            ImGui::SliderFloat("Pitch", &camera.pitchDegrees, -89.0f, 89.0f);
            ImGui::SliderFloat(
                "Speed (m/s)",
                &camera.moveSpeed,
                WorldUnits::kUiCameraMoveSpeedMinMetersPerSecond,
                WorldUnits::kUiCameraMoveSpeedMaxMetersPerSecond,
                "%.2f"
            );
            ImGui::SliderFloat("Sensitivity", &camera.mouseSensitivity, 0.01f, 1.0f);
            ImGui::SliderFloat("Fov", &camera.fovDegrees, 20.0f, 90.0f);
            ImGui::SliderFloat(
                "Near (m)",
                &camera.nearPlane,
                WorldUnits::kUiCameraNearMinMeters,
                WorldUnits::kUiCameraNearMaxMeters,
                "%.3f"
            );
            ImGui::SliderFloat(
                "Far (m)",
                &camera.farPlane,
                WorldUnits::kUiCameraFarMinMeters,
                WorldUnits::kUiCameraFarMaxMeters,
                "%.1f"
            );
        }
        ImGui::End();
    }

    bool themeChanged = false;
    if (m_showThemeWindow)
    {
        themeChanged = DrawThemeEditorWindow();
    }

    if (m_showAssetManagerWindow)
    {
        if (ImGui::Begin("Asset Manager", &m_showAssetManagerWindow))
        {
            const std::filesystem::path assetRoot = std::filesystem::path(MINIENGINE_ASSET_DIR).lexically_normal();
            std::error_code assetErrorCode;
            std::filesystem::create_directories(assetRoot, assetErrorCode);
            const std::string normalizedAssetRoot = NormalizeAssetPath(assetRoot);

            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * m_effectiveUiScale);
            const bool modelImportAvailable = ModelLoader::IsImportAvailable();
            ImGui::BeginDisabled(!modelImportAvailable);
            if (ImGui::Button("Import glTF") && modelImportAvailable)
            {
                if (const std::optional<std::string> selectedImportPath = OpenModelFileDialog(); selectedImportPath.has_value())
                {
                    if (const auto duplicate = FindDuplicateImportedModel(*selectedImportPath, assetRoot); duplicate.has_value())
                    {
                        m_pendingDuplicateImportSourcePath = duplicate->sourcePath;
                        m_pendingDuplicateImportAssetPath = duplicate->existingAssetPath;
                        m_openDuplicateImportPopup = true;
                    }
                    else
                    {
                        result.actions.importedModelRequest = EditorUiActions::ImportedModelRequest{
                            *selectedImportPath,
                            m_assetBrowserDirectory.empty() ? normalizedAssetRoot : m_assetBrowserDirectory
                        };
                    }
                }
            }
            if (!modelImportAvailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip("Model import is unavailable in this build.");
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Load Scene"))
            {
                result.actions.selectedSceneLoadPath = OpenSceneFileDialog();
            }
            ImGui::SameLine();
            if (ImGui::Button("Save Scene"))
            {
                result.actions.selectedSceneSavePath = SaveSceneFileDialog();
            }
            ImGui::PopStyleVar();

            if (m_openDuplicateImportPopup)
            {
                ImGui::OpenPopup("Duplicate Model Import");
                m_openDuplicateImportPopup = false;
            }
            if (ImGui::BeginPopupModal("Duplicate Model Import", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextWrapped("An identical model already exists in the asset library.");
                ImGui::Spacing();
                ImGui::TextWrapped("Selected File: %s", m_pendingDuplicateImportSourcePath.c_str());
                ImGui::TextWrapped("Existing Asset: %s", m_pendingDuplicateImportAssetPath.c_str());
                ImGui::Spacing();
                ImGui::TextWrapped(
                    "Importing again will place the model and its MAT assets into the current directory with a numeric suffix."
                );
                ImGui::Spacing();

                if (ImGui::Button("Import", ImVec2(120.0f * m_effectiveUiScale, 0.0f)))
                {
                    if (!m_pendingDuplicateImportSourcePath.empty())
                    {
                        result.actions.importedModelRequest = EditorUiActions::ImportedModelRequest{
                            m_pendingDuplicateImportSourcePath,
                            m_assetBrowserDirectory.empty() ? normalizedAssetRoot : m_assetBrowserDirectory
                        };
                    }
                    m_pendingDuplicateImportSourcePath.clear();
                    m_pendingDuplicateImportAssetPath.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120.0f * m_effectiveUiScale, 0.0f)))
                {
                    m_pendingDuplicateImportSourcePath.clear();
                    m_pendingDuplicateImportAssetPath.clear();
                    ImGui::CloseCurrentPopup();
                }

                ImGui::EndPopup();
            }

            if (m_openAssetRenamePopup)
            {
                ImGui::OpenPopup("Rename Asset");
                m_openAssetRenamePopup = false;
            }
            if (ImGui::BeginPopupModal("Rename Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextWrapped("Rename: %s", m_assetRenameTargetPath.c_str());
                ImGui::Spacing();
                if (m_focusAssetRenameInput)
                {
                    ImGui::SetKeyboardFocusHere();
                    m_focusAssetRenameInput = false;
                }
                const bool submitted = ImGui::InputText(
                    "##AssetRenameInput",
                    m_assetRenameBuffer.data(),
                    m_assetRenameBuffer.size(),
                    ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue
                );
                if (!m_assetRenameError.empty())
                {
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s", m_assetRenameError.c_str());
                }
                ImGui::Spacing();

                if (ImGui::Button("Rename", ImVec2(120.0f * m_effectiveUiScale, 0.0f)) || submitted)
                {
                    if (CommitAssetRename(assetRoot))
                    {
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120.0f * m_effectiveUiScale, 0.0f)))
                {
                    m_assetRenameTargetPath.clear();
                    m_assetRenameError.clear();
                    ImGui::CloseCurrentPopup();
                }

                ImGui::EndPopup();
            }

            if (m_openDeleteAssetPopup)
            {
                ImGui::OpenPopup("Confirm Delete Asset");
                m_openDeleteAssetPopup = false;
            }
            if (ImGui::BeginPopupModal("Confirm Delete Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextWrapped("Delete asset: %s", m_pendingDeleteAssetPath.c_str());
                ImGui::Spacing();
                ImGui::TextWrapped("This action cannot be undone.");
                ImGui::Spacing();

                if (ImGui::Button("Delete", ImVec2(120.0f * m_effectiveUiScale, 0.0f)))
                {
                    ConfirmRequestedAssetDelete(assetRoot, normalizedAssetRoot, result);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120.0f * m_effectiveUiScale, 0.0f)))
                {
                    m_pendingDeleteAssetPath.clear();
                    ImGui::CloseCurrentPopup();
                }

                ImGui::EndPopup();
            }

            if (m_assetBrowserDirectory.empty())
            {
                m_assetBrowserDirectory = normalizedAssetRoot;
            }

            if (!m_selectedAssetPath.empty())
            {
                std::error_code selectionErrorCode;
                const std::filesystem::path selectedPath(m_selectedAssetPath);
                if (!std::filesystem::exists(selectedPath, selectionErrorCode) ||
                    selectionErrorCode ||
                    !IsSameOrDescendantPath(selectedPath, assetRoot))
                {
                    m_selectedAssetPath.clear();
                    m_selectedAssetIsDirectory = false;
                }
            }

            {
                std::error_code browserErrorCode;
                const std::filesystem::path browserPath(m_assetBrowserDirectory);
                if (!std::filesystem::exists(browserPath, browserErrorCode) ||
                    browserErrorCode ||
                    !std::filesystem::is_directory(browserPath, browserErrorCode) ||
                    browserErrorCode ||
                    !IsSameOrDescendantPath(browserPath, assetRoot))
                {
                    m_assetBrowserDirectory = normalizedAssetRoot;
                }
            }

            const std::filesystem::path browserDirectoryPath(m_assetBrowserDirectory);
            std::vector<std::filesystem::directory_entry> browserEntries;
            CollectSortedDirectoryEntries(browserDirectoryPath, browserEntries);
            const bool assetWindowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            const bool assetPopupOpen =
                ImGui::IsPopupOpen("Rename Asset") ||
                ImGui::IsPopupOpen("Confirm Delete Asset") ||
                ImGui::IsPopupOpen("Duplicate Model Import");

            if (assetWindowFocused && !assetPopupOpen)
            {
                const bool canPasteShortcut =
                    !m_copiedAssetPath.empty() &&
                    std::filesystem::exists(std::filesystem::path(m_copiedAssetPath));
                const bool canDeleteShortcut =
                    !m_selectedAssetPath.empty() &&
                    NormalizeAssetPath(std::filesystem::path(m_selectedAssetPath)) != normalizedAssetRoot;

                if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && !m_selectedAssetPath.empty())
                {
                    m_copiedAssetPath = m_selectedAssetPath;
                }
                if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false) && canPasteShortcut)
                {
                    result.actions.pastedAsset = EditorUiActions::AssetPasteRequest{
                        m_copiedAssetPath,
                        m_assetBrowserDirectory
                    };
                }
                if (ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_N, false))
                {
                    try
                    {
                        const std::filesystem::path newFolderPath = MakeUniqueAssetFolderPath(browserDirectoryPath, "New Folder");
                        std::filesystem::create_directory(newFolderPath);
                        m_selectedAssetPath = NormalizeAssetPath(newFolderPath);
                        m_selectedAssetIsDirectory = true;
                        BeginAssetRename(m_selectedAssetPath);
                    }
                    catch (const std::exception& error)
                    {
                        m_assetRenameError = error.what();
                    }
                }
                if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && !m_selectedAssetPath.empty())
                {
                    BeginAssetRename(m_selectedAssetPath);
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && canDeleteShortcut)
                {
                    RequestAssetDelete(m_selectedAssetPath);
                }
            }

            ImGui::SeparatorText("Asset Browser");
            ImGui::TextWrapped("Root: %s", assetRoot.string().c_str());
            ImGui::TextWrapped("Current Directory: %s", BuildAssetBrowserPathLabel(assetRoot, browserDirectoryPath).c_str());
            ImGui::TextDisabled(
                "Import places models and MAT assets directly into the current directory. "
                "Use the directory tree to navigate; folders remain fully user-defined."
            );

            const bool isAtAssetRoot = NormalizeAssetPath(browserDirectoryPath) == normalizedAssetRoot;
            ImGui::BeginDisabled(isAtAssetRoot);
            if (ImGui::Button("Up One Level"))
            {
                m_assetBrowserDirectory = NormalizeAssetPath(browserDirectoryPath.parent_path());
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Go Root"))
            {
                m_assetBrowserDirectory = normalizedAssetRoot;
            }
            ImGui::SameLine();
            if (ImGui::Button("New Folder"))
            {
                try
                {
                    const std::filesystem::path newFolderPath = MakeUniqueAssetFolderPath(browserDirectoryPath, "New Folder");
                    std::filesystem::create_directory(newFolderPath);
                    m_selectedAssetPath = NormalizeAssetPath(newFolderPath);
                    m_selectedAssetIsDirectory = true;
                    BeginAssetRename(m_selectedAssetPath);
                }
                catch (const std::exception& error)
                {
                    m_assetRenameError = error.what();
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%u items", static_cast<unsigned int>(browserEntries.size()));

            const float footerReserveHeight = 56.0f * m_effectiveUiScale;
            const float browserHeight =
                std::max(ImGui::GetContentRegionAvail().y - footerReserveHeight, 260.0f * m_effectiveUiScale);
            const float browserAvailableWidth = std::max(
                ImGui::GetContentRegionAvail().x,
                420.0f * m_effectiveUiScale
            );
            const float minimumAssetPaneWidth = 180.0f * m_effectiveUiScale;
            const float minimumTreePaneWidth = 180.0f * m_effectiveUiScale;
            const float splitterWidth = std::max(6.0f * m_effectiveUiScale, 4.0f);
            const float usableBrowserWidth = std::max(
                browserAvailableWidth - splitterWidth,
                minimumAssetPaneWidth + minimumTreePaneWidth
            );
            const float minimumTreeRatio = minimumTreePaneWidth / usableBrowserWidth;
            const float maximumTreeRatio = 1.0f - (minimumAssetPaneWidth / usableBrowserWidth);
            m_assetBrowserTreePaneRatio = std::clamp(
                m_assetBrowserTreePaneRatio,
                minimumTreeRatio,
                maximumTreeRatio
            );
            float treePaneWidth = std::clamp(
                usableBrowserWidth * m_assetBrowserTreePaneRatio,
                minimumTreePaneWidth,
                usableBrowserWidth - minimumAssetPaneWidth
            );
            float assetPaneWidth = std::max(
                browserAvailableWidth - treePaneWidth - splitterWidth,
                minimumAssetPaneWidth
            );

            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(kAssetPaneBackgroundColor));
            if (ImGui::BeginChild("AssetDirectoryTreePane", ImVec2(treePaneWidth, browserHeight), true))
            {
                ImGui::TextDisabled("Directory Tree");
                ImGui::Separator();
                DrawAssetDirectoryTreeNode(
                    assetRoot,
                    assetRoot,
                    NormalizeAssetPath(browserDirectoryPath),
                    true,
                    m_selectedAssetPath,
                    m_selectedAssetIsDirectory,
                    m_assetBrowserDirectory
                );
            }
            ImGui::EndChild();

            ImGui::SameLine(0.0f, 0.0f);
            const bool draggingSplitter = DrawHorizontalSplitter("##AssetBrowserSplitter", splitterWidth, browserHeight);
            if (draggingSplitter)
            {
                treePaneWidth = std::clamp(
                    treePaneWidth + ImGui::GetIO().MouseDelta.x,
                    minimumTreePaneWidth,
                    browserAvailableWidth - splitterWidth - minimumAssetPaneWidth
                );
                m_assetBrowserTreePaneRatio = treePaneWidth / usableBrowserWidth;
                assetPaneWidth = std::max(
                    browserAvailableWidth - treePaneWidth - splitterWidth,
                    minimumAssetPaneWidth
                );
            }

            ImGui::SameLine(0.0f, 0.0f);
            if (ImGui::BeginChild("AssetListPane", ImVec2(assetPaneWidth, browserHeight), true))
            {
                if (browserEntries.empty())
                {
                    ImGui::TextDisabled("This directory is empty.");
                }
                else
                {
                    const float preferredTileWidth = 220.0f * m_effectiveUiScale;
                    const float tileSpacing = 12.0f * m_effectiveUiScale;
                    const float availableWidth = std::max(
                        ImGui::GetContentRegionAvail().x,
                        160.0f * m_effectiveUiScale
                    );
                    const int columnCount = std::max(
                        1,
                        static_cast<int>((availableWidth + tileSpacing) / (preferredTileWidth + tileSpacing))
                    );

                    if (ImGui::BeginTable(
                        "AssetTileGrid",
                        columnCount,
                        ImGuiTableFlags_SizingStretchSame |
                            ImGuiTableFlags_NoPadOuterX |
                            ImGuiTableFlags_BordersInnerV
                    ))
                    {
                        for (int columnIndex = 0; columnIndex < columnCount; ++columnIndex)
                        {
                            const std::string columnId = "##AssetTileColumn" + std::to_string(columnIndex);
                            ImGui::TableSetupColumn(columnId.c_str(), ImGuiTableColumnFlags_WidthStretch, 1.0f);
                        }

                        for (const std::filesystem::directory_entry& entry : browserEntries)
                        {
                            DrawAssetBrowserTile(
                                entry,
                                assetRoot,
                                m_effectiveUiScale,
                                requestedModelPreviewPath,
                                requestedMaterialPreviewPath,
                                result,
                                m_copiedAssetPath,
                                m_assetRenameTargetPath,
                                m_assetRenameBuffer,
                                m_assetRenameError,
                                m_openAssetRenamePopup,
                                m_focusAssetRenameInput,
                                m_pendingDeleteAssetPath,
                                m_openDeleteAssetPopup,
                                m_assetBrowserDirectory,
                                m_selectedAssetPath,
                                m_selectedAssetIsDirectory
                            );
                        }

                        ImGui::EndTable();
                    }
                }
                if (ImGui::BeginPopupContextWindow(
                    "AssetPaneContext",
                    ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems
                ))
                {
                    if (ImGui::MenuItem("New Folder"))
                    {
                        try
                        {
                            const std::filesystem::path newFolderPath = MakeUniqueAssetFolderPath(
                                browserDirectoryPath,
                                "New Folder"
                            );
                            std::filesystem::create_directory(newFolderPath);
                            m_selectedAssetPath = NormalizeAssetPath(newFolderPath);
                            m_selectedAssetIsDirectory = true;
                            BeginAssetRename(m_selectedAssetPath);
                        }
                        catch (const std::exception& error)
                        {
                            m_assetRenameError = error.what();
                        }
                    }

                    const bool canPaste =
                        !m_copiedAssetPath.empty() &&
                        std::filesystem::exists(std::filesystem::path(m_copiedAssetPath));
                    if (ImGui::MenuItem("Paste", nullptr, false, canPaste))
                    {
                        result.actions.pastedAsset = EditorUiActions::AssetPasteRequest{
                            m_copiedAssetPath,
                            m_assetBrowserDirectory
                        };
                    }
                    ImGui::EndPopup();
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();

            if (!lastLoadError.empty())
            {
                ImGui::TextWrapped("Last Error: %s", lastLoadError.c_str());
            }
            if (!lastSceneIoError.empty())
            {
                ImGui::TextWrapped("Scene Error: %s", lastSceneIoError.c_str());
            }
        }
        ImGui::End();
    }

    if (requestedModelPreviewPath.has_value())
    {
        OpenModelProcessorWindow(*requestedModelPreviewPath);
    }
    if (requestedMaterialPreviewPath.has_value())
    {
        OpenMaterialPreviewWindow(*requestedMaterialPreviewPath);
    }

    if (m_showModelProcessorWindow)
    {
        bool keepModelProcessorWindowOpen = m_showModelProcessorWindow;
        bool requestCloseModelProcessorWindow = false;
        bool requestReloadModelProcessorWindow = false;
        const std::string modelProcessorReloadPath = m_modelProcessorModelPath;

        ImGui::SetNextWindowSize(
            ImVec2(560.0f * m_effectiveUiScale, 560.0f * m_effectiveUiScale),
            ImGuiCond_FirstUseEver
        );
        if (ImGui::Begin("Model Preview", &keepModelProcessorWindowOpen))
        {
            const bool selectedAssetIsMaterial =
                !m_selectedAssetIsDirectory &&
                !m_selectedAssetPath.empty() &&
                IsMaterialAssetPath(std::filesystem::path(m_selectedAssetPath));

            ImGui::TextWrapped("Model: %s", m_modelProcessorDisplayName.empty() ? "<unknown>" : m_modelProcessorDisplayName.c_str());
            ImGui::TextWrapped("Asset Path: %s", m_modelProcessorModelPath.c_str());
            ImGui::TextWrapped(
                "Scene Target: %s",
                scene.HasSelection() ? scene.GetSelectedTag().name.c_str() : "<no entity selected>"
            );
            ImGui::TextDisabled("Double-click a glTF model in Asset Manager to open this editor.");

            if (!m_modelProcessorStatusMessage.empty())
            {
                ImGui::Spacing();
                ImGui::TextWrapped("Status: %s", m_modelProcessorStatusMessage.c_str());
            }

            ImGui::BeginDisabled(!scene.HasSelection());
            if (ImGui::Button("Load Into Selected Entity", ImVec2(220.0f * m_effectiveUiScale, 0.0f)))
            {
                result.actions.selectedModelPath = m_modelProcessorModelPath;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Reload From Disk"))
            {
                requestReloadModelProcessorWindow = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Close", ImVec2(140.0f * m_effectiveUiScale, 0.0f)))
            {
                requestCloseModelProcessorWindow = true;
            }

            if (!m_modelProcessorLoadedModel.submeshes.empty())
            {
                uint32_t previewTriangleCount = 0;
                for (const ModelSubmeshData& submesh : m_modelProcessorLoadedModel.submeshes)
                {
                    previewTriangleCount += static_cast<uint32_t>(submesh.mesh.indices.size() / 3u);
                }

                ImGui::Spacing();
                ImGui::SeparatorText("Live Draft Preview");
                DrawMaterialShadedPreview(
                    m_modelProcessorLoadedModel,
                    m_modelProcessorMaterials,
                    m_modelProcessorMaterials.empty() ? -1 : m_modelProcessorSelectedMaterialIndex,
                    m_modelPreviewYaw,
                    m_modelPreviewPitch,
                    m_modelPreviewDistance,
                    m_modelPreviewAutoFramePending,
                    m_effectiveUiScale,
                    "ModelDraftPreviewCanvas",
                    "Approximate PBR draft preview"
                );
                ImGui::Text(
                    "Submeshes: %u  Triangles: %u",
                    static_cast<unsigned int>(m_modelProcessorLoadedModel.submeshes.size()),
                    static_cast<unsigned int>(previewTriangleCount)
                );

                ImGui::Spacing();
                ImGui::SeparatorText("UV Preview");
                DrawModelUvPreview(
                    m_modelProcessorLoadedModel,
                    m_modelProcessorSelectedUvSubmeshIndex,
                    m_effectiveUiScale
                );
            }

            if (m_modelProcessorMaterials.empty())
            {
                ImGui::Spacing();
                ImGui::TextDisabled("No material slots are available right now.");
            }
            else
            {
                m_modelProcessorSelectedMaterialIndex = std::clamp(
                    m_modelProcessorSelectedMaterialIndex,
                    0,
                    static_cast<int>(m_modelProcessorMaterials.size()) - 1
                );

                const auto buildCurrentSlotLabel = [&]() -> std::string
                {
                    return BuildMaterialSlotLabel(
                        m_modelProcessorMaterials[static_cast<size_t>(m_modelProcessorSelectedMaterialIndex)],
                        static_cast<size_t>(m_modelProcessorSelectedMaterialIndex)
                    );
                };
                const std::string currentSlotLabel = buildCurrentSlotLabel();

                if (ImGui::BeginCombo("Material Slot", currentSlotLabel.c_str()))
                {
                    for (size_t materialIndex = 0; materialIndex < m_modelProcessorMaterials.size(); ++materialIndex)
                    {
                        const bool isSelected = static_cast<int>(materialIndex) == m_modelProcessorSelectedMaterialIndex;
                        const std::string label =
                            BuildMaterialSlotLabel(m_modelProcessorMaterials[materialIndex], materialIndex);
                        if (ImGui::Selectable(label.c_str(), isSelected))
                        {
                            m_modelProcessorSelectedMaterialIndex = static_cast<int>(materialIndex);
                        }
                        if (isSelected)
                        {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }

                const size_t selectedMaterialIndex = static_cast<size_t>(m_modelProcessorSelectedMaterialIndex);
                ModelImportedMaterialInfo& selectedMaterial = m_modelProcessorMaterials[selectedMaterialIndex];
                const std::string slotMaterialAssetPath =
                    selectedMaterialIndex < m_modelProcessorMaterialAssetPaths.size()
                    ? m_modelProcessorMaterialAssetPaths[selectedMaterialIndex]
                    : std::string{};

                ImGui::TextWrapped(
                    "Slot Material Asset: %s",
                    slotMaterialAssetPath.empty() ? "<generated on save>" : slotMaterialAssetPath.c_str()
                );
                ImGui::Text("Draft State: %s", m_modelProcessorDirty ? "Modified" : "Clean");
                ImGui::TextWrapped(
                    "Selected MAT Asset: %s",
                    selectedAssetIsMaterial ? m_selectedAssetPath.c_str() : "<select a MAT asset in Asset Manager>"
                );

                ImGui::BeginDisabled(!selectedAssetIsMaterial);
                if (ImGui::Button("Apply Selected MAT To Slot", ImVec2(220.0f * m_effectiveUiScale, 0.0f)))
                {
                    try
                    {
                        const YAML::Node root = YAML::LoadFile(m_selectedAssetPath);
                        const std::string fallbackMaterialName =
                            BuildMaterialSlotLabel(selectedMaterial, selectedMaterialIndex);
                        selectedMaterial = ReadImportedMaterialInfoFromYamlNode(
                            root["material"],
                            fallbackMaterialName
                        );
                        m_modelProcessorDirty = true;
                        m_modelProcessorStatusMessage = "Loaded material draft from asset: " + m_selectedAssetPath;
                    }
                    catch (const std::exception& error)
                    {
                        m_modelProcessorStatusMessage = error.what();
                    }
                }
                ImGui::EndDisabled();

                bool materialChanged = false;
                EnsureMaterialShaderGraph(selectedMaterial.name, std::nullopt, selectedMaterial);
                if (m_materialGraphSelectedNodeId != 0 &&
                    FindMaterialGraphNode(selectedMaterial.shaderGraph, m_materialGraphSelectedNodeId) == nullptr)
                {
                    m_materialGraphSelectedNodeId = 0;
                }
                if (m_materialGraphNodeResizeActive &&
                    FindMaterialGraphNode(selectedMaterial.shaderGraph, m_materialGraphResizeNodeId) == nullptr)
                {
                    m_materialGraphNodeResizeActive = false;
                    m_materialGraphResizeNodeId = 0;
                    m_materialGraphResizeEdges = 0;
                }
                if (m_materialGraphSelectedLinkId != 0 &&
                    FindMaterialGraphLink(selectedMaterial.shaderGraph, m_materialGraphSelectedLinkId) == nullptr)
                {
                    m_materialGraphSelectedLinkId = 0;
                }

                ImGui::Spacing();
                ImGui::SeparatorText("Shader Node Graph");
                ImGui::TextWrapped("Left click selects nodes and links. Selected nodes can be moved by dragging empty space, resized from highlighted edges, and edited from the right-click card menu. Middle mouse pans the canvas, and the wheel zooms around the cursor.");

                const MaterialShaderNode* selectedGraphNodeForActions =
                    FindMaterialGraphNode(selectedMaterial.shaderGraph, m_materialGraphSelectedNodeId);
                const MaterialShaderLink* selectedGraphLinkForActions =
                    FindMaterialGraphLink(selectedMaterial.shaderGraph, m_materialGraphSelectedLinkId);

                if (ImGui::Button("Add Node", ImVec2(140.0f * m_effectiveUiScale, 0.0f)))
                {
                    m_openMaterialGraphAddNodePopup = true;
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(
                    selectedGraphNodeForActions == nullptr ||
                    selectedGraphNodeForActions->type == MaterialShaderNodeType::Output
                );
                if (ImGui::Button("Delete Selected Node", ImVec2(190.0f * m_effectiveUiScale, 0.0f)))
                {
                    RemoveMaterialGraphNode(selectedMaterial.shaderGraph, m_materialGraphSelectedNodeId);
                    if (m_materialGraphNodeResizeActive &&
                        m_materialGraphResizeNodeId == m_materialGraphSelectedNodeId)
                    {
                        m_materialGraphNodeResizeActive = false;
                        m_materialGraphResizeNodeId = 0;
                        m_materialGraphResizeEdges = 0;
                    }
                    if (m_materialGraphLinkDragActive &&
                        m_materialGraphLinkDragFromNodeId == m_materialGraphSelectedNodeId)
                    {
                        m_materialGraphLinkDragActive = false;
                        m_materialGraphLinkDragFromNodeId = 0;
                        m_materialGraphLinkDragFromSlot.clear();
                    }
                    m_materialGraphSelectedNodeId = 0;
                    m_materialGraphSelectedLinkId = 0;
                    materialChanged = true;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(selectedGraphLinkForActions == nullptr);
                if (ImGui::Button("Delete Selected Link", ImVec2(180.0f * m_effectiveUiScale, 0.0f)))
                {
                    RemoveMaterialGraphLink(selectedMaterial.shaderGraph, m_materialGraphSelectedLinkId);
                    m_materialGraphSelectedLinkId = 0;
                    materialChanged = true;
                }
                ImGui::EndDisabled();
                if (m_materialGraphLinkDragActive)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled(
                        "Linking: %u.%s",
                        static_cast<unsigned int>(m_materialGraphLinkDragFromNodeId),
                        m_materialGraphLinkDragFromSlot.c_str()
                    );
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel Link", ImVec2(140.0f * m_effectiveUiScale, 0.0f)))
                    {
                        m_materialGraphLinkDragActive = false;
                        m_materialGraphLinkDragFromNodeId = 0;
                        m_materialGraphLinkDragFromSlot.clear();
                    }
                }

                const MaterialShaderNode* selectedGraphNode =
                    FindMaterialGraphNode(selectedMaterial.shaderGraph, m_materialGraphSelectedNodeId);
                const MaterialShaderLink* selectedGraphLink =
                    FindMaterialGraphLink(selectedMaterial.shaderGraph, m_materialGraphSelectedLinkId);
                if (selectedGraphNode != nullptr)
                {
                    ImGui::TextWrapped(
                        "Selected Node: %s (%s)",
                        selectedGraphNode->name.empty()
                            ? GetDefaultMaterialGraphNodeName(selectedGraphNode->type)
                            : selectedGraphNode->name.c_str(),
                        GetMaterialGraphNodeTypeLabel(selectedGraphNode->type)
                    );
                }
                else if (selectedGraphLink != nullptr)
                {
                    const MaterialShaderNode* fromNode =
                        FindMaterialGraphNode(selectedMaterial.shaderGraph, selectedGraphLink->fromNodeId);
                    const MaterialShaderNode* toNode =
                        FindMaterialGraphNode(selectedMaterial.shaderGraph, selectedGraphLink->toNodeId);
                    ImGui::TextWrapped(
                        "Selected Link: %s.%s -> %s.%s",
                        fromNode != nullptr ? fromNode->name.c_str() : "<missing>",
                        selectedGraphLink->fromSlot.c_str(),
                        toNode != nullptr ? toNode->name.c_str() : "<missing>",
                        selectedGraphLink->toSlot.c_str()
                    );
                }
                else
                {
                    ImGui::TextDisabled("Tip: right-click the graph background or use Add Node to expand the material graph.");
                }

                if (ImGui::BeginChild(
                        "MaterialShaderGraphCanvas",
                        ImVec2(0.0f, 660.0f * m_effectiveUiScale),
                        true,
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                    ))
                {
                    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
                    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
                    ImGui::SetItemKeyOwner(ImGuiKey_MouseMiddle);

                    const ImVec2 canvasOrigin =
                        ImVec2(
                            ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x,
                            ImGui::GetWindowPos().y + ImGui::GetWindowContentRegionMin().y
                        );
                    const ImVec2 canvasMax =
                        ImVec2(
                            ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x,
                            ImGui::GetWindowPos().y + ImGui::GetWindowContentRegionMax().y
                        );
                    const float visibleWidth = canvasMax.x - canvasOrigin.x;
                    const float visibleHeight = canvasMax.y - canvasOrigin.y;
                    const float gridStep = 48.0f * m_effectiveUiScale * m_materialGraphZoom;
                    const float nodeUiScale = m_effectiveUiScale * m_materialGraphZoom;
                    const bool canPasteClipboardNode =
                        CanPasteMaterialGraphNode(selectedMaterial.shaderGraph, m_materialGraphClipboardNode);
                    const bool mouseOverGraphNode = IsMouseOverMaterialGraphNode(
                        selectedMaterial.shaderGraph,
                        ImGui::GetIO().MousePos,
                        canvasOrigin,
                        m_materialGraphViewOrigin,
                        nodeUiScale,
                        m_materialGraphZoom
                    );
                    const ImGuiHoveredFlags canvasHoverFlags =
                        ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_ChildWindows;
                    const bool canvasHovered = ImGui::IsWindowHovered(canvasHoverFlags);
                    const bool canvasBackgroundHovered = canvasHovered && !mouseOverGraphNode;
                    const ImGuiID canvasInputOwner = ImGui::GetCurrentWindow()->ID;
                    if (canvasHovered || m_materialGraphPanningActive)
                    {
                        ImGui::SetKeyOwner(
                            ImGuiKey_MouseWheelY,
                            canvasInputOwner,
                            ImGuiInputFlags_LockThisFrame
                        );
                        ImGui::SetKeyOwner(
                            ImGuiKey_MouseWheelX,
                            canvasInputOwner,
                            ImGuiInputFlags_LockThisFrame
                        );
                        ImGui::SetKeyOwner(
                            ImGuiKey_MouseMiddle,
                            canvasInputOwner,
                            ImGuiInputFlags_LockUntilRelease
                        );
                        ImGui::SetNextFrameWantCaptureMouse(true);
                    }
                    if (canvasBackgroundHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
                    {
                        m_materialGraphPanningActive = true;
                    }
                    if (m_materialGraphPanningActive)
                    {
                        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle))
                        {
                            m_materialGraphViewOrigin.x -= ImGui::GetIO().MouseDelta.x / m_materialGraphZoom;
                            m_materialGraphViewOrigin.y -= ImGui::GetIO().MouseDelta.y / m_materialGraphZoom;
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        }
                        else
                        {
                            m_materialGraphPanningActive = false;
                        }
                    }
                    if (canvasBackgroundHovered && std::abs(ImGui::GetIO().MouseWheel) > 0.0f)
                    {
                        const ImVec2 mousePosition = ImGui::GetIO().MousePos;
                        const MaterialGraphNodePosition graphPositionBeforeZoom =
                            ComputeMaterialGraphPositionFromScreen(
                                mousePosition,
                                canvasOrigin,
                                m_materialGraphViewOrigin,
                                m_materialGraphZoom
                            );
                        const float zoomFactor = std::pow(kMaterialGraphZoomStep, ImGui::GetIO().MouseWheel);
                        m_materialGraphZoom = std::clamp(
                            m_materialGraphZoom * zoomFactor,
                            kMaterialGraphMinZoom,
                            kMaterialGraphMaxZoom
                        );
                        m_materialGraphViewOrigin.x =
                            graphPositionBeforeZoom.x -
                            (mousePosition.x - canvasOrigin.x) / m_materialGraphZoom;
                        m_materialGraphViewOrigin.y =
                            graphPositionBeforeZoom.y -
                            (mousePosition.y - canvasOrigin.y) / m_materialGraphZoom;
                    }

                    if (m_openMaterialGraphAddNodePopup)
                    {
                        m_materialGraphContextSpawnPosition = MaterialGraphNodePosition{
                            m_materialGraphViewOrigin.x + visibleWidth * 0.28f / m_materialGraphZoom,
                            m_materialGraphViewOrigin.y + visibleHeight * 0.22f / m_materialGraphZoom
                        };
                        ImGui::OpenPopup("MaterialGraphAddNodePopup");
                        m_openMaterialGraphAddNodePopup = false;
                    }

                    if (canvasHovered &&
                        !mouseOverGraphNode &&
                        !ImGui::IsAnyItemHovered() &&
                        ImGui::IsMouseReleased(ImGuiMouseButton_Right))
                    {
                        m_materialGraphContextSpawnPosition = ComputeMaterialGraphPositionFromScreen(
                            ImGui::GetIO().MousePos,
                            canvasOrigin,
                            m_materialGraphViewOrigin,
                            m_materialGraphZoom
                        );
                        ImGui::OpenPopup("MaterialGraphAddNodePopup");
                    }

                    ImDrawList* graphDrawList = ImGui::GetWindowDrawList();
                    graphDrawList->PushClipRect(canvasOrigin, canvasMax, true);
                    const float gridOffsetX =
                        std::fmod(-(m_materialGraphViewOrigin.x * m_materialGraphZoom), gridStep);
                    for (float x = gridOffsetX; x < canvasMax.x - canvasOrigin.x; x += gridStep)
                    {
                        graphDrawList->AddLine(
                            ImVec2(canvasOrigin.x + x, canvasOrigin.y),
                            ImVec2(canvasOrigin.x + x, canvasMax.y),
                            IM_COL32(44, 54, 70, 90),
                            1.0f
                        );
                    }
                    const float gridOffsetY =
                        std::fmod(-(m_materialGraphViewOrigin.y * m_materialGraphZoom), gridStep);
                    for (float y = gridOffsetY; y < canvasMax.y - canvasOrigin.y; y += gridStep)
                    {
                        graphDrawList->AddLine(
                            ImVec2(canvasOrigin.x, canvasOrigin.y + y),
                            ImVec2(canvasMax.x, canvasOrigin.y + y),
                            IM_COL32(44, 54, 70, 90),
                            1.0f
                        );
                    }

                    uint32_t pendingDeleteNodeId = 0;
                    bool connectionCompletedThisFrame = false;
                    bool graphNodeCapturedMouse = false;
                    std::optional<MaterialGraphNodePosition> pendingPasteNodePosition;
                    std::vector<MaterialGraphRenderedPin> renderedPins;
                    renderedPins.reserve(selectedMaterial.shaderGraph.nodes.size() * 8u);

                    if (m_materialGraphNodeResizeActive)
                    {
                        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
                        {
                            m_materialGraphNodeResizeActive = false;
                            m_materialGraphResizeNodeId = 0;
                            m_materialGraphResizeEdges = 0;
                        }
                        else if (MaterialShaderNode* resizingNode =
                                     FindMaterialGraphNode(selectedMaterial.shaderGraph, m_materialGraphResizeNodeId);
                                 resizingNode != nullptr)
                        {
                            const ImVec2 resizeMouseDelta(
                                ImGui::GetIO().MousePos.x - m_materialGraphResizeStartMouse.x,
                                ImGui::GetIO().MousePos.y - m_materialGraphResizeStartMouse.y
                            );
                            materialChanged |= ApplyMaterialGraphNodeResize(
                                *resizingNode,
                                m_materialGraphResizeEdges,
                                m_materialGraphResizeStartPosition,
                                m_materialGraphResizeStartSize,
                                resizeMouseDelta,
                                m_effectiveUiScale,
                                m_materialGraphZoom
                            );
                            graphNodeCapturedMouse = true;
                            ImGui::SetMouseCursor(GetMaterialGraphResizeCursor(m_materialGraphResizeEdges));
                        }
                        else
                        {
                            m_materialGraphNodeResizeActive = false;
                            m_materialGraphResizeNodeId = 0;
                            m_materialGraphResizeEdges = 0;
                        }
                    }

                    for (MaterialShaderNode& node : selectedMaterial.shaderGraph.nodes)
                    {
                        MaterialGraphNodeDrawResult drawResult = DrawMaterialGraphNode(
                            selectedMaterial,
                            node,
                            m_modelProcessorModelPath,
                            static_cast<uint32_t>(selectedMaterialIndex),
                            canvasOrigin,
                            m_materialGraphViewOrigin,
                            nodeUiScale,
                            m_materialGraphZoom,
                            m_materialGraphSelectedNodeId == node.id,
                            m_materialGraphLinkDragActive,
                            m_materialGraphNodeResizeActive && m_materialGraphResizeNodeId == node.id,
                            canPasteClipboardNode,
                            m_materialGraphLinkDragFromNodeId,
                            m_materialGraphLinkDragFromSlot,
                            &m_modelProcessorStatusMessage
                        );
                        materialChanged |= drawResult.changed;
                        graphNodeCapturedMouse |= drawResult.capturesMouse;
                        if (drawResult.selected)
                        {
                            m_materialGraphSelectedNodeId = node.id;
                            if (drawResult.selectedLinkId == 0)
                            {
                                m_materialGraphSelectedLinkId = 0;
                            }
                        }
                        if (drawResult.selectedLinkId != 0)
                        {
                            m_materialGraphSelectedLinkId = drawResult.selectedLinkId;
                        }
                        if (drawResult.requestDelete)
                        {
                            pendingDeleteNodeId = node.id;
                        }
                        if (drawResult.requestCopy)
                        {
                            m_materialGraphClipboardNode = node;
                            m_modelProcessorStatusMessage =
                                "Copied " + std::string(GetMaterialGraphNodeTypeLabel(node.type)) + " node.";
                        }
                        if (drawResult.requestPaste)
                        {
                            pendingPasteNodePosition = drawResult.pastePosition;
                        }
                        if (drawResult.requestStartResize)
                        {
                            m_materialGraphNodeResizeActive = true;
                            m_materialGraphResizeNodeId = node.id;
                            m_materialGraphResizeEdges = drawResult.resizeEdges;
                            m_materialGraphResizeStartPosition = node.position;
                            m_materialGraphResizeStartSize = GetMaterialGraphNodeLogicalSize(node);
                            m_materialGraphResizeStartMouse = ImGui::GetIO().MousePos;
                            m_materialGraphSelectedNodeId = node.id;
                            m_materialGraphSelectedLinkId = 0;
                            if (m_materialGraphLinkDragActive)
                            {
                                m_materialGraphLinkDragActive = false;
                                m_materialGraphLinkDragFromNodeId = 0;
                                m_materialGraphLinkDragFromSlot.clear();
                            }
                            graphNodeCapturedMouse = true;
                        }
                        if (drawResult.requestStartLinkDrag)
                        {
                            m_materialGraphLinkDragActive = true;
                            m_materialGraphLinkDragFromNodeId = drawResult.startLinkNodeId;
                            m_materialGraphLinkDragFromSlot = drawResult.startLinkSlot;
                            m_materialGraphNodeResizeActive = false;
                            m_materialGraphResizeNodeId = 0;
                            m_materialGraphResizeEdges = 0;
                            m_materialGraphSelectedLinkId = 0;
                        }
                        if (drawResult.connectedLinkId != 0)
                        {
                            connectionCompletedThisFrame = true;
                            m_materialGraphLinkDragActive = false;
                            m_materialGraphLinkDragFromNodeId = 0;
                            m_materialGraphLinkDragFromSlot.clear();
                            m_materialGraphSelectedLinkId = drawResult.connectedLinkId;
                        }
                        renderedPins.insert(
                            renderedPins.end(),
                            drawResult.pins.begin(),
                            drawResult.pins.end()
                        );
                    }

                    if (pendingDeleteNodeId != 0)
                    {
                        RemoveMaterialGraphNode(selectedMaterial.shaderGraph, pendingDeleteNodeId);
                        if (m_materialGraphSelectedNodeId == pendingDeleteNodeId)
                        {
                            m_materialGraphSelectedNodeId = 0;
                        }
                        if (m_materialGraphLinkDragActive &&
                            m_materialGraphLinkDragFromNodeId == pendingDeleteNodeId)
                        {
                            m_materialGraphLinkDragActive = false;
                            m_materialGraphLinkDragFromNodeId = 0;
                            m_materialGraphLinkDragFromSlot.clear();
                        }
                        if (m_materialGraphNodeResizeActive &&
                            m_materialGraphResizeNodeId == pendingDeleteNodeId)
                        {
                            m_materialGraphNodeResizeActive = false;
                            m_materialGraphResizeNodeId = 0;
                            m_materialGraphResizeEdges = 0;
                        }
                        if (m_materialGraphSelectedLinkId != 0 &&
                            FindMaterialGraphLink(selectedMaterial.shaderGraph, m_materialGraphSelectedLinkId) == nullptr)
                        {
                            m_materialGraphSelectedLinkId = 0;
                        }
                        materialChanged = true;
                    }

                    for (const MaterialShaderLink& link : selectedMaterial.shaderGraph.links)
                    {
                        const MaterialGraphRenderedPin* fromPin =
                            FindRenderedMaterialGraphPin(renderedPins, link.fromNodeId, link.fromSlot, false);
                        const MaterialGraphRenderedPin* toPin =
                            FindRenderedMaterialGraphPin(renderedPins, link.toNodeId, link.toSlot, true);
                        if (fromPin == nullptr || toPin == nullptr)
                        {
                            continue;
                        }

                        const ImU32 linkColor =
                            m_materialGraphSelectedLinkId == link.id
                            ? kSelectionOutlineColor
                            : GetMaterialGraphPinColor(fromPin->kind);
                        DrawNodeConnection(
                            graphDrawList,
                            fromPin->center,
                            toPin->center,
                            linkColor,
                            m_materialGraphSelectedLinkId == link.id
                                ? 3.6f * nodeUiScale
                                : 2.6f * nodeUiScale
                        );
                    }

                    if (m_materialGraphLinkDragActive)
                    {
                        const MaterialGraphRenderedPin* dragFromPin = FindRenderedMaterialGraphPin(
                            renderedPins,
                            m_materialGraphLinkDragFromNodeId,
                            m_materialGraphLinkDragFromSlot,
                            false
                        );
                        if (dragFromPin != nullptr)
                        {
                            DrawNodeConnection(
                                graphDrawList,
                                dragFromPin->center,
                                ImGui::GetIO().MousePos,
                                kSelectionOutlineColor,
                                2.4f * nodeUiScale
                            );
                        }
                        else
                        {
                            m_materialGraphLinkDragActive = false;
                            m_materialGraphLinkDragFromNodeId = 0;
                            m_materialGraphLinkDragFromSlot.clear();
                        }
                    }
                    graphDrawList->PopClipRect();

                    if (canvasBackgroundHovered &&
                        !graphNodeCapturedMouse &&
                        !ImGui::IsAnyItemHovered() &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    {
                        m_materialGraphSelectedNodeId = 0;
                        m_materialGraphSelectedLinkId = 0;
                    }

                    if (m_materialGraphLinkDragActive &&
                        ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                        !connectionCompletedThisFrame)
                    {
                        m_materialGraphLinkDragActive = false;
                        m_materialGraphLinkDragFromNodeId = 0;
                        m_materialGraphLinkDragFromSlot.clear();
                    }

                    if (pendingPasteNodePosition.has_value() && canPasteClipboardNode && m_materialGraphClipboardNode.has_value())
                    {
                        if (MaterialShaderNode* pastedNode = PasteMaterialGraphNode(
                                selectedMaterial.shaderGraph,
                                *m_materialGraphClipboardNode,
                                *pendingPasteNodePosition
                            );
                            pastedNode != nullptr)
                        {
                            m_materialGraphSelectedNodeId = pastedNode->id;
                            m_materialGraphSelectedLinkId = 0;
                            materialChanged = true;
                            m_modelProcessorStatusMessage =
                                "Pasted " + std::string(GetMaterialGraphNodeTypeLabel(pastedNode->type)) + " node copy.";
                        }
                        pendingPasteNodePosition.reset();
                    }

                    if (ImGui::BeginPopup("MaterialGraphAddNodePopup"))
                    {
                        const auto addGraphNode = [&](MaterialShaderNodeType type)
                        {
                            if (MaterialShaderNode* newNode = AddMaterialGraphNode(
                                    selectedMaterial.shaderGraph,
                                    type,
                                    m_materialGraphContextSpawnPosition
                                );
                                newNode != nullptr)
                            {
                                m_materialGraphSelectedNodeId = newNode->id;
                                m_materialGraphSelectedLinkId = 0;
                                materialChanged = true;
                                m_modelProcessorStatusMessage =
                                    "Added " + std::string(GetMaterialGraphNodeTypeLabel(type)) + " node.";
                            }
                            ImGui::CloseCurrentPopup();
                        };

                        if (ImGui::MenuItem("Texture Node"))
                        {
                            addGraphNode(MaterialShaderNodeType::Texture);
                        }
                        if (ImGui::MenuItem("Scalar Node"))
                        {
                            addGraphNode(MaterialShaderNodeType::Scalar);
                        }
                        if (ImGui::MenuItem("Color Node"))
                        {
                            addGraphNode(MaterialShaderNodeType::Color);
                        }
                        if (ImGui::MenuItem("Surface Node"))
                        {
                            addGraphNode(MaterialShaderNodeType::Surface);
                        }
                        if (ImGui::MenuItem("Blend Node"))
                        {
                            addGraphNode(MaterialShaderNodeType::Blend);
                        }
                        ImGui::BeginDisabled(MaterialGraphHasOutputNode(selectedMaterial.shaderGraph));
                        if (ImGui::MenuItem("Output Node"))
                        {
                            addGraphNode(MaterialShaderNodeType::Output);
                        }
                        ImGui::EndDisabled();
                        ImGui::Separator();
                        if (ImGui::BeginMenu("Edit"))
                        {
                            if (ImGui::MenuItem("Paste Node", nullptr, false, canPasteClipboardNode))
                            {
                                pendingPasteNodePosition = m_materialGraphContextSpawnPosition;
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndMenu();
                        }
                        ImGui::EndPopup();
                    }
                    if (pendingPasteNodePosition.has_value() && canPasteClipboardNode && m_materialGraphClipboardNode.has_value())
                    {
                        if (MaterialShaderNode* pastedNode = PasteMaterialGraphNode(
                                selectedMaterial.shaderGraph,
                                *m_materialGraphClipboardNode,
                                *pendingPasteNodePosition
                            );
                            pastedNode != nullptr)
                        {
                            m_materialGraphSelectedNodeId = pastedNode->id;
                            m_materialGraphSelectedLinkId = 0;
                            materialChanged = true;
                            m_modelProcessorStatusMessage =
                                "Pasted " + std::string(GetMaterialGraphNodeTypeLabel(pastedNode->type)) + " node copy.";
                        }
                        pendingPasteNodePosition.reset();
                    }
                }
                ImGui::EndChild();

                if (materialChanged)
                {
                    const MaterialGraphCompileResult compileResult =
                        CompileMaterialShaderGraph(selectedMaterial);
                    m_modelProcessorDirty = true;
                    if (!compileResult.message.empty())
                    {
                        m_modelProcessorStatusMessage = compileResult.message;
                    }
                }

                ImGui::Spacing();
                ImGui::SeparatorText("Resolved Material");
                const MaterialTextureBlendGraph& blendGraph = selectedMaterial.blendGraph;
                DrawPrimaryMaterialTextureRows(selectedMaterial);
                ImGui::Text(
                    "Metallic %.2f  Roughness %.2f  Normal %.2f  AO %.2f  Emissive %.2f  Opacity %.2f",
                    selectedMaterial.pbr.metallicFactor,
                    selectedMaterial.pbr.roughnessFactor,
                    selectedMaterial.pbr.normalScale,
                    selectedMaterial.pbr.occlusionStrength,
                    selectedMaterial.pbr.emissiveIntensity,
                    selectedMaterial.pbr.opacity
                );
                if (HasSecondaryMaterialLayer(blendGraph))
                {
                    ImGui::Separator();
                    DrawSecondaryMaterialTextureRows(blendGraph);
                }

                ImGui::Spacing();
                ImGui::BeginDisabled(!m_modelProcessorDirty);
                if (ImGui::Button("Save Material Graph", ImVec2(220.0f * m_effectiveUiScale, 0.0f)))
                {
                    result.actions.updatedImportedModelMaterials = EditorUiActions::ImportedModelMaterialsUpdate{
                        m_modelProcessorModelPath,
                        m_modelProcessorMaterials
                    };
                    m_modelProcessorDirty = false;
                    m_modelProcessorStatusMessage = "Saved material graph for slot: " + currentSlotLabel;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Close", ImVec2(140.0f * m_effectiveUiScale, 0.0f)))
                {
                    requestCloseModelProcessorWindow = true;
                }
            }
        }
        ImGui::End();

        if (requestReloadModelProcessorWindow && !requestCloseModelProcessorWindow && keepModelProcessorWindowOpen)
        {
            OpenModelProcessorWindow(modelProcessorReloadPath);
        }
        if (!keepModelProcessorWindowOpen || requestCloseModelProcessorWindow)
        {
            CloseModelProcessorWindow();
        }
    }

    if (m_showMaterialPreviewWindow)
    {
        bool keepMaterialPreviewWindowOpen = m_showMaterialPreviewWindow;
        bool requestCloseMaterialPreviewWindow = false;
        bool requestReloadMaterialPreviewWindow = false;
        bool requestOpenOwningModelProcessor = false;
        const std::string materialPreviewReloadPath = m_materialPreviewAssetPath;
        const std::string owningModelPath = m_materialPreviewModelPath;
        const int owningMaterialIndex = m_materialPreviewMaterialIndex;

        ImGui::SetNextWindowSize(
            ImVec2(520.0f * m_effectiveUiScale, 520.0f * m_effectiveUiScale),
            ImGuiCond_FirstUseEver
        );
        if (ImGui::Begin("Material Preview", &keepMaterialPreviewWindowOpen))
        {
            ImGui::TextWrapped(
                "Material: %s",
                m_materialPreviewMaterial.name.empty() ? m_materialPreviewDisplayName.c_str() : m_materialPreviewMaterial.name.c_str()
            );
            ImGui::TextWrapped("Asset Path: %s", m_materialPreviewAssetPath.c_str());
            ImGui::TextWrapped("Owning Model: %s", owningModelPath.empty() ? "<unknown>" : owningModelPath.c_str());
            ImGui::Text("Material Slot: %d", owningMaterialIndex);

            if (!m_materialPreviewStatusMessage.empty())
            {
                ImGui::Spacing();
                ImGui::TextWrapped("Status: %s", m_materialPreviewStatusMessage.c_str());
            }

            if (!owningModelPath.empty())
            {
                if (ImGui::Button("Open Owning Model Preview", ImVec2(240.0f * m_effectiveUiScale, 0.0f)))
                {
                    requestOpenOwningModelProcessor = true;
                }
                ImGui::SameLine();
            }
            if (ImGui::Button("Reload From Disk"))
            {
                requestReloadMaterialPreviewWindow = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Close", ImVec2(140.0f * m_effectiveUiScale, 0.0f)))
            {
                requestCloseMaterialPreviewWindow = true;
            }

            ImGui::Spacing();
            ImGui::SeparatorText("Primary PBR Textures");
            const MaterialTextureBlendGraph& blendGraph = m_materialPreviewMaterial.blendGraph;
            DrawPrimaryMaterialTextureRows(m_materialPreviewMaterial);

            ImGui::SeparatorText("PBR Factors");
            ImGui::Text(
                "Base Factor: %.2f %.2f %.2f %.2f",
                m_materialPreviewMaterial.pbr.baseColorFactor[0],
                m_materialPreviewMaterial.pbr.baseColorFactor[1],
                m_materialPreviewMaterial.pbr.baseColorFactor[2],
                m_materialPreviewMaterial.pbr.baseColorFactor[3]
            );
            ImGui::Text(
                "Emissive: %.2f %.2f %.2f  Intensity: %.2f",
                m_materialPreviewMaterial.pbr.emissiveColor[0],
                m_materialPreviewMaterial.pbr.emissiveColor[1],
                m_materialPreviewMaterial.pbr.emissiveColor[2],
                m_materialPreviewMaterial.pbr.emissiveIntensity
            );
            ImGui::Text(
                "Metallic: %.2f  Roughness: %.2f  Normal: %.2f  AO: %.2f  Opacity: %.2f",
                m_materialPreviewMaterial.pbr.metallicFactor,
                m_materialPreviewMaterial.pbr.roughnessFactor,
                m_materialPreviewMaterial.pbr.normalScale,
                m_materialPreviewMaterial.pbr.occlusionStrength,
                m_materialPreviewMaterial.pbr.opacity
            );

            if (HasSecondaryMaterialLayer(blendGraph))
            {
                ImGui::SeparatorText("Blend And Secondary Layer");
                ImGui::Text("Blend Enabled: %s", blendGraph.enabled ? "Yes" : "No");
                ImGui::Text("Blend Factor: %.2f", blendGraph.blendFactor);
                DrawSecondaryMaterialTextureRows(blendGraph);
            }
        }
        ImGui::End();

        if (requestReloadMaterialPreviewWindow && !requestCloseMaterialPreviewWindow && keepMaterialPreviewWindowOpen)
        {
            OpenMaterialPreviewWindow(materialPreviewReloadPath);
        }
        if (requestOpenOwningModelProcessor && !owningModelPath.empty())
        {
            OpenModelProcessorWindow(owningModelPath);
            if (!m_modelProcessorMaterials.empty())
            {
                m_modelProcessorSelectedMaterialIndex = std::clamp(
                    owningMaterialIndex,
                    0,
                    static_cast<int>(m_modelProcessorMaterials.size()) - 1
                );
            }
        }
        if (!keepMaterialPreviewWindowOpen || requestCloseMaterialPreviewWindow)
        {
            CloseMaterialPreviewWindow();
        }
    }

    if (m_showInputMonitorWindow)
    {
        ImGui::SetNextWindowSize(
            ImVec2(720.0f * m_effectiveUiScale, 360.0f * m_effectiveUiScale),
            ImGuiCond_FirstUseEver
        );
        if (ImGui::Begin("Input Monitor", &m_showInputMonitorWindow))
        {
            const std::vector<std::string> inputMessages = Log::GetInputMessagesSnapshot();
            ImGui::Text("Captured Events: %u", static_cast<unsigned int>(inputMessages.size()));
            ImGui::SameLine();
            if (ImGui::Button("Clear"))
            {
                Log::ClearInputMessages();
            }
            ImGui::SameLine();
            ImGui::Checkbox("Auto-scroll", &m_inputMonitorAutoScroll);
            ImGui::Separator();

            if (ImGui::BeginChild("InputMonitorLog", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_HorizontalScrollbar))
            {
                const bool shouldAutoScroll =
                    m_inputMonitorAutoScroll &&
                    ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
                for (const std::string& message : inputMessages)
                {
                    ImGui::TextUnformatted(message.c_str());
                }

                if (shouldAutoScroll)
                {
                    ImGui::SetScrollHereY(1.0f);
                }
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }

    if (m_showSceneWindow)
    {
        if (ImGui::Begin("Scene", &m_showSceneWindow))
        {
            ImGui::TextWrapped(
                "Selection: %s",
                scene.HasSelection() ? "Selected by model center in viewport" : "Click a model center in the viewport to select"
            );
            ImGui::Text("Entities: %u", static_cast<unsigned int>(scene.GetEntityOrder().size()));
            if (ImGui::Button("Add Entity"))
            {
                result.actions.createSceneEntity = true;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!scene.HasSelection());
            if (ImGui::Button("Delete Entity"))
            {
                result.actions.deleteSelectedSceneEntity = true;
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            for (entt::entity entity : scene.GetEntityOrder())
            {
                const TagComponent& tag = scene.GetTag(entity);
                if (ImGui::Selectable(tag.name.c_str(), scene.IsSelected(entity)))
                {
                    scene.SetSelectedEntity(entity);
                }
            }

            if (scene.HasSelection())
            {
                TagComponent& tag = scene.GetSelectedTag();
                TransformComponent& transform = scene.GetSelectedTransform();
                ModelComponent& model = scene.GetSelectedModel();
                GizmoSettings& gizmo = scene.GetGizmoSettings();

                ImGui::Separator();
                ImGui::TextWrapped("Controller Target: %s", model.displayName.c_str());
                ImGui::TextWrapped("Controller Mode: viewport gizmo");
                char tagBuffer[128]{};
                std::snprintf(tagBuffer, sizeof(tagBuffer), "%s", tag.name.c_str());
                if (ImGui::InputText("TagComponent", tagBuffer, sizeof(tagBuffer)))
                {
                    tag.name = tagBuffer;
                }

                if (ImGui::CollapsingHeader("TransformComponent", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    DrawTransformComponent(transform);
                    if (ImGui::Button("Reset Transform"))
                    {
                        scene.ResetSelectedTransform();
                    }
                }

                if (ImGui::CollapsingHeader("ModelComponent", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    ImGui::TextWrapped("Display Name: %s", model.displayName.c_str());
                    ImGui::TextWrapped("Source Path: %s", model.sourcePath.empty() ? "<builtin cube>" : model.sourcePath.c_str());
                    ImGui::Text("Imported Unit Scale: 1.0 = 1 meter");
                    ImGui::Text("Submeshes: %u", model.submeshCount);

                    if (model.hasBounds)
                    {
                        ImGui::Text("Bounds Min (m): %.2f %.2f %.2f", model.minBounds.x, model.minBounds.y, model.minBounds.z);
                        ImGui::Text("Bounds Max (m): %.2f %.2f %.2f", model.maxBounds.x, model.maxBounds.y, model.maxBounds.z);
                    }

                    if (!model.sourcePath.empty())
                    {
                        ImGui::TextWrapped(
                            "Materials and PBR textures are edited from Asset Manager. Double-click the glTF model to open Model Preview, or double-click a MAT asset to inspect one material."
                        );
                    }

                    DrawImportedModelInspector(model);
                }

                if (ImGui::CollapsingHeader("GizmoComponent", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    DrawGizmoControls(gizmo);
                }

                ImGui::Separator();
                ImGui::TextWrapped("Default Config: %s", scene.GetConfigPath().empty() ? "<not loaded>" : scene.GetConfigPath().c_str());
                ImGui::TextWrapped("Scene File: %s", scene.GetSceneFilePath().empty() ? "<unsaved>" : scene.GetSceneFilePath().c_str());
                const std::string yamlPreview = scene.BuildSceneYamlPreview();
                ImGui::InputTextMultiline(
                    "Scene YAML",
                    const_cast<char*>(yamlPreview.c_str()),
                    yamlPreview.size() + 1,
                    ImVec2(-FLT_MIN, 180.0f),
                    ImGuiInputTextFlags_ReadOnly
                );
            }
            else
            {
                ImGui::TextUnformatted("No entity selected.");
            }
        }
        ImGui::End();
    }

    if (m_showViewportWindow)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        if (ImGui::Begin("Viewport", &m_showViewportWindow))
        {
            const bool flipViewportImageY = false;
            const ViewportOverlayRect viewportRect = BuildViewportOverlayRect(viewportTextureId, flipViewportImageY);
            DrawViewportOverlay(viewportRect, viewportTextureId);
            result.viewportExtent = BuildViewportExtent(viewportRect);
            result.viewportInteractionRect = SDL_FRect{
                viewportRect.origin.x,
                viewportRect.origin.y,
                viewportRect.size.x,
                viewportRect.size.y
            };
            result.viewportAllowsMouseInteraction = viewportRect.size.x > 0.0f && viewportRect.size.y > 0.0f;
            HandleViewportAssetDropTarget(result, camera, viewportRect, result.viewportExtent, currentBackendType);
            HandleViewportShortcuts(scene, camera, viewportRect);
            RefreshViewportMatrices(camera, matrices, scene, result.viewportExtent, currentBackendType);
            DrawViewManipulator(camera, matrices, viewportRect);
            RefreshViewportMatrices(camera, matrices, scene, result.viewportExtent, currentBackendType);
            DrawGizmoOverlay(scene, matrices, viewportRect);
            const std::vector<ProjectedEntityCenter> projectedCenters = ProjectSceneCenters(scene, matrices, viewportRect);
            HandleViewportSelection(scene, projectedCenters, viewportRect);
            DrawViewportSelectionOverlay(scene, matrices, viewportRect, projectedCenters);
            ImGui::SetCursorScreenPos(ImVec2(viewportRect.origin.x + 12.0f, viewportRect.origin.y + 12.0f));
            ImGui::BeginGroup();
            ImGui::TextUnformatted("Viewport");
            ImGui::TextUnformatted("F to frame, W/E/R to switch gizmo, drag assets here to place");
            ImGui::Text("Render Size: %u x %u", result.viewportExtent.width, result.viewportExtent.height);
            ImGui::Text("Viewport FPS: %.1f", ImGui::GetIO().Framerate);
            ImGui::EndGroup();
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    result.engineSettingsChanged =
        themeChanged ||
        std::abs(previousUiScale - m_uiScale) > 0.0001f ||
        previousShowCameraWindow != m_showCameraWindow ||
        previousShowAssetManagerWindow != m_showAssetManagerWindow ||
        previousShowInputMonitorWindow != m_showInputMonitorWindow ||
        previousShowSceneWindow != m_showSceneWindow ||
        previousShowThemeWindow != m_showThemeWindow ||
        previousShowViewportWindow != m_showViewportWindow;

    return result;
}

void EditorUiController::ApplyEngineSettings(const EngineSettings& settings)
{
    m_uiScale = platform::ui::ResolveConfiguredUiScale(settings.editorUi.scale);
    m_showCameraWindow = settings.editorUi.windows.camera;
    m_showAssetManagerWindow = settings.editorUi.windows.assetManager;
    m_showInputMonitorWindow = settings.editorUi.windows.inputMonitor;
    m_showSceneWindow = settings.editorUi.windows.scene;
    m_showThemeWindow = settings.editorUi.windows.theme;
    m_showViewportWindow = settings.editorUi.windows.viewport;

    if (settings.editorUi.theme.hasCustomColors)
    {
        ImGuiStyle& style = ImGui::GetStyle();
        for (int colorIndex = 0; colorIndex < ImGuiCol_COUNT; ++colorIndex)
        {
            if (!settings.editorUi.theme.colorDefined[static_cast<size_t>(colorIndex)])
            {
                continue;
            }
            style.Colors[colorIndex] = settings.editorUi.theme.colors[static_cast<size_t>(colorIndex)];
        }
        SyncBaseStyleColorsFromCurrentStyle();
    }
}

void EditorUiController::WriteEngineSettings(EngineSettings& settings) const
{
    settings.version = 1;
    platform::ui::SetConfiguredUiScaleForCurrentPlatform(settings.editorUi.scale, m_uiScale);
    settings.editorUi.windows.camera = m_showCameraWindow;
    settings.editorUi.windows.assetManager = m_showAssetManagerWindow;
    settings.editorUi.windows.inputMonitor = m_showInputMonitorWindow;
    settings.editorUi.windows.scene = m_showSceneWindow;
    settings.editorUi.windows.theme = m_showThemeWindow;
    settings.editorUi.windows.viewport = m_showViewportWindow;
    settings.editorUi.theme.hasCustomColors = true;

    const ImGuiStyle& style = ImGui::GetStyle();
    for (int colorIndex = 0; colorIndex < ImGuiCol_COUNT; ++colorIndex)
    {
        settings.editorUi.theme.colors[static_cast<size_t>(colorIndex)] = style.Colors[colorIndex];
        settings.editorUi.theme.colorDefined[static_cast<size_t>(colorIndex)] = true;
    }
}

void EditorUiController::CaptureDefaultThemeColors()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    for (int colorIndex = 0; colorIndex < ImGuiCol_COUNT; ++colorIndex)
    {
        m_defaultThemeColors[static_cast<size_t>(colorIndex)] = style.Colors[colorIndex];
    }
}

void EditorUiController::SyncBaseStyleColorsFromCurrentStyle()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    for (int colorIndex = 0; colorIndex < ImGuiCol_COUNT; ++colorIndex)
    {
        m_baseStyle.Colors[colorIndex] = style.Colors[colorIndex];
    }
}

void EditorUiController::ResetThemeColorsToDefault()
{
    ImGuiStyle& style = ImGui::GetStyle();
    for (int colorIndex = 0; colorIndex < ImGuiCol_COUNT; ++colorIndex)
    {
        style.Colors[colorIndex] = m_defaultThemeColors[static_cast<size_t>(colorIndex)];
    }
    SyncBaseStyleColorsFromCurrentStyle();
}

bool EditorUiController::DrawThemeEditorWindow()
{
    if (!ImGui::Begin("Theme", &m_showThemeWindow))
    {
        ImGui::End();
        return false;
    }

    ImGui::TextWrapped("Edit the editor palette live. Changes apply immediately and remain stable when UI scale changes.");
    ImGui::Separator();

    bool changed = false;
    if (ImGui::Button("Reset Theme Colors"))
    {
        ResetThemeColorsToDefault();
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Re-capture Current As Default"))
    {
        CaptureDefaultThemeColors();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Tip: right-click a color swatch for copy/paste or manual hex input.");

    changed |= DrawThemeColorSection("Surfaces", kThemeSurfaceEntries.data(), kThemeSurfaceEntries.size());
    changed |= DrawThemeColorSection("Controls", kThemeControlEntries.data(), kThemeControlEntries.size());
    changed |= DrawThemeColorSection("Chrome", kThemeChromeEntries.data(), kThemeChromeEntries.size());
    changed |= DrawThemeColorSection("Tabs And Docking", kThemeTabDockEntries.data(), kThemeTabDockEntries.size());
    changed |= DrawThemeColorSection("States", kThemeStateEntries.data(), kThemeStateEntries.size());
    changed |= DrawThemeColorSection("Feedback", kThemeFeedbackEntries.data(), kThemeFeedbackEntries.size());
    changed |= DrawThemeColorSection("Feedback Active", kThemeFeedbackActiveEntries.data(), kThemeFeedbackActiveEntries.size());

    if (changed)
    {
        SyncBaseStyleColorsFromCurrentStyle();
    }

    ImGui::End();
    return changed;
}

void EditorUiController::ApplyUiScale()
{
    ImGuiStyle& style = ImGui::GetStyle();
    m_effectiveUiScale = platform::ui::ClampUiScale(platform::ui::ResolveWindowUiScale(m_window) * m_uiScale);

    if (std::abs(style.FontScaleMain - m_effectiveUiScale) <= 0.001f)
    {
        return;
    }

    style = m_baseStyle;
    style.ScaleAllSizes(m_effectiveUiScale);
    style.FontScaleMain = m_effectiveUiScale;
}
