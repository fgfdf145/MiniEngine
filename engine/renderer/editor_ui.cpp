#include "editor_ui.h"
#include "model_loader.h"

#include <editor_scene.h>
#include <file_dialog/file_dialog.h>
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
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>
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

ModelImportedMaterialInfo BuildImportedMaterialInfo(const ModelMaterialData& material)
{
    return ModelImportedMaterialInfo{
        material.name,
        material.baseColorTexturePath,
        material.normalTexturePath,
        material.metallicTexturePath,
        material.roughnessTexturePath,
        material.occlusionTexturePath,
        material.emissiveTexturePath,
        material.blendGraph
    };
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

    if (const YAML::Node graphNode = materialNode["texture_graph"]; graphNode && graphNode.IsMap())
    {
        material.blendGraph.enabled = graphNode["enabled"].as<bool>(material.blendGraph.enabled);
        material.blendGraph.blendFactor = graphNode["blend_factor"].as<float>(material.blendGraph.blendFactor);
        material.blendGraph.blendMaskTexturePath =
            graphNode["blend_mask_texture_path"].as<std::string>(material.blendGraph.blendMaskTexturePath);
        material.blendGraph.secondaryBaseColorTexturePath =
            graphNode["secondary_base_color_texture_path"].as<std::string>(
                material.blendGraph.secondaryBaseColorTexturePath
            );
        material.blendGraph.secondaryNormalTexturePath =
            graphNode["secondary_normal_texture_path"].as<std::string>(material.blendGraph.secondaryNormalTexturePath);
        material.blendGraph.secondaryMetallicTexturePath =
            graphNode["secondary_metallic_texture_path"].as<std::string>(
                material.blendGraph.secondaryMetallicTexturePath
            );
        material.blendGraph.secondaryRoughnessTexturePath =
            graphNode["secondary_roughness_texture_path"].as<std::string>(
                material.blendGraph.secondaryRoughnessTexturePath
            );
        material.blendGraph.secondaryOcclusionTexturePath =
            graphNode["secondary_occlusion_texture_path"].as<std::string>(
                material.blendGraph.secondaryOcclusionTexturePath
            );
        material.blendGraph.secondaryEmissiveTexturePath =
            graphNode["secondary_emissive_texture_path"].as<std::string>(
                material.blendGraph.secondaryEmissiveTexturePath
            );
    }

    material.blendGraph.blendFactor = std::clamp(material.blendGraph.blendFactor, 0.0f, 1.0f);
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
    ImGui::TextWrapped("FBX texture bindings are applied only when the submesh carries valid UVs.");

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
            const bool hasProgrammableGraph =
                material.blendGraph.enabled || CountMaterialGraphSecondaryTextures(material.blendGraph) > 0;
            const std::string materialName = material.name.empty()
                ? ("Material " + std::to_string(materialIndex))
                : material.name;
            const std::string treeLabel =
                (hasProgrammableGraph ? "[PBR Graph] " : "") + materialName + "##material_" + std::to_string(materialIndex);
            if (!ImGui::TreeNode(treeLabel.c_str()))
            {
                continue;
            }

            DrawTexturePathRow("Base Color", material.baseColorTexturePath);
            DrawTexturePathRow("Normal", material.normalTexturePath);
            DrawTexturePathRow("Metallic", material.metallicTexturePath);
            DrawTexturePathRow("Roughness", material.roughnessTexturePath);
            DrawTexturePathRow("Occlusion", material.occlusionTexturePath);
            DrawTexturePathRow("Emissive", material.emissiveTexturePath);
            if (hasProgrammableGraph)
            {
                ImGui::Separator();
                ImGui::Text("Programmable Blend: %s", material.blendGraph.enabled ? "Enabled" : "Prepared");
                ImGui::Text("Blend Factor: %.2f", material.blendGraph.blendFactor);
                DrawTexturePathRow("Blend Mask", material.blendGraph.blendMaskTexturePath);
                DrawTexturePathRow("Layer B Base", material.blendGraph.secondaryBaseColorTexturePath);
                DrawTexturePathRow("Layer B Normal", material.blendGraph.secondaryNormalTexturePath);
                DrawTexturePathRow("Layer B Metallic", material.blendGraph.secondaryMetallicTexturePath);
                DrawTexturePathRow("Layer B Roughness", material.blendGraph.secondaryRoughnessTexturePath);
                DrawTexturePathRow("Layer B Occlusion", material.blendGraph.secondaryOcclusionTexturePath);
                DrawTexturePathRow("Layer B Emissive", material.blendGraph.secondaryEmissiveTexturePath);
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

bool DrawGraphTextureSlotEditor(const char* label, const char* idSuffix, std::string& path)
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
            path = *selectedPath;
            changed = true;
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

    const std::filesystem::path assetModelsRoot = assetRoot / "models";
    if (!std::filesystem::exists(assetModelsRoot))
    {
        return std::nullopt;
    }

    std::error_code errorCode;
    for (std::filesystem::recursive_directory_iterator iterator(assetModelsRoot, errorCode);
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

std::string BuildAssetTypeLabel(const std::filesystem::path& path, bool isDirectory)
{
    if (isDirectory)
    {
        return "DIR";
    }

    const std::string extension = ToLowerCopy(path.extension().string());
    if (IsSupportedModelAssetPath(path))
    {
        return "FBX";
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

void DrawAssetDirectoryTreeNode(
    const std::filesystem::path& path,
    std::string& browserDirectory,
    std::string& selectedAssetPath,
    bool& selectedAssetIsDirectory,
    bool defaultOpen = false
)
{
    const std::string normalizedPath = NormalizeAssetPath(path);
    const std::string nodeLabel = path.filename().empty() ? path.string() : path.filename().string();
    const bool isCurrentDirectory = browserDirectory == normalizedPath;
    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow |
        ImGuiTreeNodeFlags_SpanAvailWidth |
        ImGuiTreeNodeFlags_FramePadding;
    if (isCurrentDirectory)
    {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (defaultOpen || (!browserDirectory.empty() && IsSameOrDescendantPath(std::filesystem::path(browserDirectory), path)))
    {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    }

    const bool open = ImGui::TreeNodeEx(normalizedPath.c_str(), flags, "%s", nodeLabel.c_str());
    if (ImGui::IsItemClicked())
    {
        browserDirectory = normalizedPath;
        selectedAssetPath = normalizedPath;
        selectedAssetIsDirectory = true;
    }

    if (!open)
    {
        return;
    }

    std::vector<std::filesystem::directory_entry> entries;
    CollectSortedDirectoryEntries(path, entries);
    for (const std::filesystem::directory_entry& entry : entries)
    {
        std::error_code errorCode;
        if (entry.is_directory(errorCode) && !errorCode)
        {
            DrawAssetDirectoryTreeNode(entry.path(), browserDirectory, selectedAssetPath, selectedAssetIsDirectory);
        }
    }
    ImGui::TreePop();
}

void DrawAssetBrowserTile(
    const std::filesystem::directory_entry& entry,
    const std::filesystem::path& assetRoot,
    float tileWidth,
    float tileHeight,
    float effectiveUiScale,
    bool hasSceneSelection,
    EditorUiFrameResult& result,
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
    const bool canLoadSelectedModel = !isDirectory && hasSceneSelection && IsSupportedModelAssetPath(path);
    const std::string typeLabel = BuildAssetTypeLabel(path, isDirectory);
    const std::string displayName = path.filename().string();
    const std::string subtitle =
        isDirectory
        ? "Double-click to open"
        : (canLoadSelectedModel ? "Double-click to load" : BuildAssetBrowserPathLabel(assetRoot, path.parent_path()));

    ImGui::TableNextColumn();
    ImGui::PushID(normalizedPath.c_str());
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
        else if (canLoadSelectedModel)
        {
            result.actions.selectedModelPath = normalizedPath;
        }
    }

    if (hovered)
    {
        ImGui::SetTooltip("%s", path.string().c_str());
    }

    if (!isDirectory && IsSupportedModelAssetPath(path) && ImGui::BeginDragDropSource())
    {
        const std::string payloadPath = normalizedPath;
        ImGui::SetDragDropPayload(
            kAssetModelDragDropPayloadId,
            payloadPath.c_str(),
            payloadPath.size() + 1
        );
        ImGui::TextUnformatted("Place FBX In Scene");
        ImGui::TextWrapped("%s", displayName.c_str());
        ImGui::EndDragDropSource();
    }

    ImGui::PopID();
}

void DrawTopToolbar(
    bool& showCameraWindow,
    bool& showAssetManagerWindow,
    bool& showSceneWindow,
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
    ImGui::BeginDisabled(showSceneWindow);
    if (ImGui::Button("Scene"))
    {
        showSceneWindow = true;
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
        showSceneWindow = true;
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
    const EditorScene& scene,
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
    const EditorScene& scene,
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
    const EditorScene& scene,
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
    const EditorScene& scene,
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
    const EditorScene& scene,
    RenderExtent viewportExtent,
    RenderBackendType currentBackendType
)
{
    const bool useVulkanClipSpace = currentBackendType == RenderBackendType::Vulkan;
    matrices.view = camera.GetViewMatrix();
    matrices.projection = camera.GetProjectionMatrix(viewportExtent, false, useVulkanClipSpace);
    matrices.renderProjection = camera.GetProjectionMatrix(viewportExtent, useVulkanClipSpace, useVulkanClipSpace);
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
    const bool useZeroToOneDepth = currentBackendType == RenderBackendType::Vulkan;
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

void HandleViewportShortcuts(EditorScene& scene, Camera& camera, const ViewportOverlayRect& viewportRect)
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
    EditorScene& scene,
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

void DrawGizmoOverlay(EditorScene& scene, ViewportMatrices& matrices, const ViewportOverlayRect& viewportRect)
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

void EditorUiController::BeginFrame(SDL_Window* window)
{
    m_window = window;
    if (!m_hasCapturedBaseStyle)
    {
        m_baseStyle = ImGui::GetStyle();
        m_hasCapturedBaseStyle = true;
    }

    ApplyUiScale();
    ImGuizmo::BeginFrame();
}

void EditorUiController::OpenModelProcessorWindow(const std::string& modelPath)
{
    const std::filesystem::path normalizedPath = NormalizeFilesystemPath(modelPath);

    m_showModelProcessorWindow = true;
    m_modelProcessorModelPath = normalizedPath.string();
    m_modelProcessorDisplayName = normalizedPath.filename().string();
    m_modelProcessorStatusMessage.clear();
    m_modelProcessorSelectedMaterialIndex = 0;
    m_modelProcessorDirty = false;
    m_modelProcessorMaterials.clear();
    m_modelProcessorMaterialAssetPaths.clear();

    try
    {
        const LoadedModelData loadedModel = ModelLoader::LoadModel(m_modelProcessorModelPath);
        m_modelProcessorMaterials.reserve(loadedModel.materials.size());
        for (const ModelMaterialData& material : loadedModel.materials)
        {
            m_modelProcessorMaterials.push_back(BuildImportedMaterialInfo(material));
        }

        if (m_modelProcessorMaterials.empty())
        {
            m_modelProcessorMaterials.push_back(ModelImportedMaterialInfo{});
        }

        m_modelProcessorMaterialAssetPaths =
            LoadImportedModelMaterialAssetPaths(normalizedPath, m_modelProcessorMaterials.size());
    }
    catch (const std::exception& error)
    {
        m_modelProcessorStatusMessage = error.what();
    }
}

void EditorUiController::CloseModelProcessorWindow()
{
    m_showModelProcessorWindow = false;
    m_modelProcessorModelPath.clear();
    m_modelProcessorDisplayName.clear();
    m_modelProcessorStatusMessage.clear();
    m_modelProcessorMaterials.clear();
    m_modelProcessorMaterialAssetPaths.clear();
    m_modelProcessorSelectedMaterialIndex = 0;
    m_modelProcessorDirty = false;
}

EditorUiFrameResult EditorUiController::Draw(
    Camera& camera,
    ViewportMatrices& matrices,
    EditorScene& scene,
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

    const float toolbarHeight = 44.0f * m_effectiveUiScale;
    DrawDockspaceBelowToolbar(toolbarHeight);

    DrawTopToolbar(
        m_showCameraWindow,
        m_showAssetManagerWindow,
        m_showSceneWindow,
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
            ImGui::Text("Window DPI Scale: %.2f x", GetWindowUiScale());
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

    if (m_showAssetManagerWindow)
    {
        if (ImGui::Begin("Asset Manager", &m_showAssetManagerWindow))
        {
            const std::filesystem::path assetRoot = std::filesystem::path(MINIENGINE_ASSET_DIR).lexically_normal();
            std::error_code assetErrorCode;
            std::filesystem::create_directories(assetRoot, assetErrorCode);
            const std::string normalizedAssetRoot = NormalizeAssetPath(assetRoot);

            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * m_effectiveUiScale);
            if (ImGui::Button("Import FBX"))
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
                        result.actions.importedModelSourcePath = *selectedImportPath;
                    }
                }
            }
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
                ImGui::TextWrapped("Importing again will continue and add a numeric suffix to the imported asset bundle.");
                ImGui::Spacing();

                if (ImGui::Button("Import", ImVec2(120.0f * m_effectiveUiScale, 0.0f)))
                {
                    if (!m_pendingDuplicateImportSourcePath.empty())
                    {
                        result.actions.importedModelSourcePath = m_pendingDuplicateImportSourcePath;
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

            ImGui::SeparatorText("Asset Browser");
            ImGui::TextWrapped("Root: %s", assetRoot.string().c_str());
            ImGui::TextWrapped("Current Folder: %s", BuildAssetBrowserPathLabel(assetRoot, browserDirectoryPath).c_str());
            ImGui::TextWrapped("Current Model: %s", currentModelPath.empty() ? "<builtin cube>" : currentModelPath.c_str());
            ImGui::TextWrapped("Current Scene: %s", scene.GetSceneFilePath().empty() ? "<unsaved>" : scene.GetSceneFilePath().c_str());
            ImGui::TextDisabled("Double-click folder to open, double-click model to edit, drag model row into viewport.");

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
            ImGui::TextDisabled("%u items", static_cast<unsigned int>(browserEntries.size()));

            const float footerReserveHeight = 235.0f * m_effectiveUiScale;
            const float browserHeight =
                std::max(ImGui::GetContentRegionAvail().y - footerReserveHeight, 260.0f * m_effectiveUiScale);
            if (ImGui::BeginChild("AssetListPane", ImVec2(0.0f, browserHeight), true))
            {
                if (browserEntries.empty())
                {
                    ImGui::TextDisabled("This folder is empty.");
                }
                else if (ImGui::BeginTable(
                    "AssetListTable",
                    3,
                    ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_Borders |
                    ImGuiTableFlags_Resizable |
                    ImGuiTableFlags_SizingStretchProp |
                    ImGuiTableFlags_ScrollY
                ))
                {
                    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 72.0f * m_effectiveUiScale);
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.55f);
                    ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthStretch, 0.45f);
                    ImGui::TableHeadersRow();

                    for (const std::filesystem::directory_entry& entry : browserEntries)
                    {
                        std::error_code entryErrorCode;
                        const std::filesystem::path path = entry.path();
                        const std::string normalizedPath = NormalizeAssetPath(path);
                        const bool isDirectory = entry.is_directory(entryErrorCode) && !entryErrorCode;
                        const bool isSelected = m_selectedAssetPath == normalizedPath;
                        const std::string typeLabel = BuildAssetTypeLabel(path, isDirectory);
                        const std::string displayName = path.filename().string();
                        const std::string locationLabel = BuildAssetBrowserPathLabel(assetRoot, path.parent_path());

                        ImGui::TableNextRow();
                        ImGui::PushID(normalizedPath.c_str());

                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(typeLabel.c_str());

                        ImGui::TableSetColumnIndex(1);
                        if (ImGui::Selectable(displayName.c_str(), isSelected, ImGuiSelectableFlags_AllowDoubleClick))
                        {
                            m_selectedAssetPath = normalizedPath;
                            m_selectedAssetIsDirectory = isDirectory;

                            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                            {
                                if (isDirectory)
                                {
                                    m_assetBrowserDirectory = normalizedPath;
                                }
                                else if (IsSupportedModelAssetPath(path))
                                {
                                    OpenModelProcessorWindow(normalizedPath);
                                }
                            }
                        }
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::SetTooltip("%s", normalizedPath.c_str());
                        }
                        if (!isDirectory && IsSupportedModelAssetPath(path) && ImGui::BeginDragDropSource())
                        {
                            ImGui::SetDragDropPayload(
                                kAssetModelDragDropPayloadId,
                                normalizedPath.c_str(),
                                normalizedPath.size() + 1
                            );
                            ImGui::TextUnformatted("Place Model In Scene");
                            ImGui::TextWrapped("%s", displayName.c_str());
                            ImGui::EndDragDropSource();
                        }

                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextWrapped("%s", locationLabel.c_str());

                        ImGui::PopID();
                    }

                    ImGui::EndTable();
                }
            }
            ImGui::EndChild();

            ImGui::SeparatorText("Selection");
            if (m_selectedAssetPath.empty())
            {
                ImGui::TextUnformatted("Select an asset from the list above.");
            }
            else
            {
                const std::filesystem::path selectedAssetPath(m_selectedAssetPath);
                const bool selectedIsModel = !m_selectedAssetIsDirectory && IsSupportedModelAssetPath(selectedAssetPath);
                const bool canLoadSelectedModel = scene.HasSelection() && selectedIsModel;
                const bool canDeleteSelectedAsset = selectedAssetPath != assetRoot;

                ImGui::TextWrapped("Path: %s", m_selectedAssetPath.c_str());
                ImGui::Text("Type: %s", m_selectedAssetIsDirectory ? "Folder" : BuildAssetTypeLabel(selectedAssetPath, false).c_str());

                if (m_selectedAssetIsDirectory)
                {
                    ImGui::BeginDisabled(m_assetBrowserDirectory == m_selectedAssetPath);
                    if (ImGui::Button("Open Folder"))
                    {
                        m_assetBrowserDirectory = m_selectedAssetPath;
                    }
                    ImGui::EndDisabled();
                }
                else
                {
                    if (ImGui::Button("Open Containing Folder"))
                    {
                        m_assetBrowserDirectory = NormalizeAssetPath(selectedAssetPath.parent_path());
                    }
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!selectedIsModel);
                    if (ImGui::Button("Model Settings"))
                    {
                        OpenModelProcessorWindow(m_selectedAssetPath);
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!canLoadSelectedModel);
                    if (ImGui::Button("Load To Selected"))
                    {
                        result.actions.selectedModelPath = m_selectedAssetPath;
                    }
                    ImGui::EndDisabled();
                }

                ImGui::SameLine();
                ImGui::BeginDisabled(!canDeleteSelectedAsset);
                if (ImGui::Button("Delete Selected"))
                {
                    result.actions.deleteAssetPath = m_selectedAssetPath;
                    if (IsSameOrDescendantPath(browserDirectoryPath, selectedAssetPath))
                    {
                        const std::filesystem::path fallbackDirectory = selectedAssetPath.parent_path().empty()
                            ? assetRoot
                            : selectedAssetPath.parent_path();
                        m_assetBrowserDirectory = IsSameOrDescendantPath(fallbackDirectory, assetRoot)
                            ? NormalizeAssetPath(fallbackDirectory)
                            : normalizedAssetRoot;
                    }
                    m_selectedAssetPath.clear();
                    m_selectedAssetIsDirectory = false;
                }
                ImGui::EndDisabled();
            }

            ImGui::SeparatorText("Selected Model UV");
            if (scene.HasSelection())
            {
                const ModelComponent& selectedModel = scene.GetSelectedModel();
                ImGui::TextWrapped("Target: %s", selectedModel.displayName.c_str());
                DrawSelectedModelUvTextureControls(&selectedModel, result);
            }
            else
            {
                DrawSelectedModelUvTextureControls(nullptr, result);
            }
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
        if (ImGui::Begin("Model Processor", &keepModelProcessorWindowOpen))
        {
            const bool selectedAssetIsMaterial =
                !m_selectedAssetIsDirectory &&
                !m_selectedAssetPath.empty() &&
                IsMaterialAssetPath(std::filesystem::path(m_selectedAssetPath));

            ImGui::TextWrapped("Model: %s", m_modelProcessorDisplayName.empty() ? "<unknown>" : m_modelProcessorDisplayName.c_str());
            ImGui::TextWrapped("Asset Path: %s", m_modelProcessorModelPath.c_str());
            ImGui::TextWrapped(
                "Selected MAT Asset: %s",
                selectedAssetIsMaterial ? m_selectedAssetPath.c_str() : "<select a MAT asset in Asset Manager>"
            );
            ImGui::TextDisabled("Save will rewrite this model asset and refresh all scene instances using it.");

            if (!m_modelProcessorStatusMessage.empty())
            {
                ImGui::Spacing();
                ImGui::TextWrapped("Status: %s", m_modelProcessorStatusMessage.c_str());
            }

            if (m_modelProcessorMaterials.empty())
            {
                ImGui::Spacing();
                ImGui::TextDisabled("No material slots are available right now.");
                if (ImGui::Button("Reload From Disk"))
                {
                    requestReloadModelProcessorWindow = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Close", ImVec2(140.0f * m_effectiveUiScale, 0.0f)))
                {
                    requestCloseModelProcessorWindow = true;
                }
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

                ImGui::BeginDisabled(!selectedAssetIsMaterial);
                if (ImGui::Button("Apply Selected MAT To Slot"))
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
                ImGui::SameLine();
                if (ImGui::Button("Reload From Disk"))
                {
                    requestReloadModelProcessorWindow = true;
                }

                ImGui::Spacing();
                ImGui::SeparatorText("Material Preview");
                DrawTexturePathRow("Base Color", selectedMaterial.baseColorTexturePath);
                DrawTexturePathRow("Normal", selectedMaterial.normalTexturePath);
                DrawTexturePathRow("Metallic", selectedMaterial.metallicTexturePath);
                DrawTexturePathRow("Roughness", selectedMaterial.roughnessTexturePath);
                DrawTexturePathRow("Occlusion", selectedMaterial.occlusionTexturePath);
                DrawTexturePathRow("Emissive", selectedMaterial.emissiveTexturePath);

                const bool hasProgrammableGraph =
                    selectedMaterial.blendGraph.enabled ||
                    CountMaterialGraphSecondaryTextures(selectedMaterial.blendGraph) > 0;
                if (hasProgrammableGraph)
                {
                    ImGui::Separator();
                    ImGui::Text("Programmable Blend: %s", selectedMaterial.blendGraph.enabled ? "Enabled" : "Prepared");
                    ImGui::Text("Blend Factor: %.2f", selectedMaterial.blendGraph.blendFactor);
                    DrawTexturePathRow("Blend Mask", selectedMaterial.blendGraph.blendMaskTexturePath);
                    DrawTexturePathRow("Layer B Base", selectedMaterial.blendGraph.secondaryBaseColorTexturePath);
                    DrawTexturePathRow("Layer B Normal", selectedMaterial.blendGraph.secondaryNormalTexturePath);
                    DrawTexturePathRow("Layer B Metallic", selectedMaterial.blendGraph.secondaryMetallicTexturePath);
                    DrawTexturePathRow("Layer B Roughness", selectedMaterial.blendGraph.secondaryRoughnessTexturePath);
                    DrawTexturePathRow("Layer B Occlusion", selectedMaterial.blendGraph.secondaryOcclusionTexturePath);
                    DrawTexturePathRow("Layer B Emissive", selectedMaterial.blendGraph.secondaryEmissiveTexturePath);
                }

                ImGui::Spacing();
                ImGui::BeginDisabled(!m_modelProcessorDirty);
                if (ImGui::Button("Save And Apply", ImVec2(180.0f * m_effectiveUiScale, 0.0f)))
                {
                    result.actions.updatedImportedModelMaterials = EditorUiActions::ImportedModelMaterialsUpdate{
                        m_modelProcessorModelPath,
                        m_modelProcessorMaterials
                    };
                    requestCloseModelProcessorWindow = true;
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

    if (m_showSceneWindow)
    {
        if (ImGui::Begin("Scene", &m_showSceneWindow))
        {
            ImGui::TextWrapped(
                "Selection: %s",
                scene.HasSelection() ? "Selected by model center in viewport" : "Click a model center in the viewport to select"
            );
            ImGui::Text("Entities: %u", static_cast<unsigned int>(scene.GetEntityOrder().size()));
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
                if (model.sourcePath.empty() || model.importedMaterials.empty())
                {
                    m_materialGraphDraftModelPath.clear();
                    m_materialGraphNodeLayoutKey.clear();
                    m_materialGraphDraftIndex = -1;
                    m_materialGraphDraggingNodeIndex = -1;
                    m_materialGraphSelectedIndex = 0;
                    m_materialGraphDraftDirty = false;
                    m_materialGraphNodeLayoutInitialized = false;
                    m_materialGraphDraft = ModelImportedMaterialInfo{};
                }
                else
                {
                    m_materialGraphSelectedIndex = std::clamp(
                        m_materialGraphSelectedIndex,
                        0,
                        static_cast<int>(model.importedMaterials.size()) - 1
                    );
                    const bool draftSourceChanged =
                        m_materialGraphDraftModelPath != model.sourcePath ||
                        m_materialGraphDraftIndex != m_materialGraphSelectedIndex;
                    if (draftSourceChanged || !m_materialGraphDraftDirty)
                    {
                        m_materialGraphDraftModelPath = model.sourcePath;
                        m_materialGraphDraftIndex = m_materialGraphSelectedIndex;
                        m_materialGraphDraft = model.importedMaterials[static_cast<size_t>(m_materialGraphSelectedIndex)];
                        if (draftSourceChanged)
                        {
                            m_materialGraphDraftDirty = false;
                        }
                    }

                    const std::string materialGraphLayoutKey =
                        model.sourcePath + "#" + std::to_string(m_materialGraphSelectedIndex);
                    if (!m_materialGraphNodeLayoutInitialized || m_materialGraphNodeLayoutKey != materialGraphLayoutKey)
                    {
                        m_materialGraphNodeLayout = BuildDefaultMaterialGraphNodeLayout();
                        m_materialGraphDraggingNodeIndex = -1;
                        m_materialGraphNodeLayoutKey = materialGraphLayoutKey;
                        m_materialGraphNodeLayoutInitialized = true;
                    }
                }

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
                    DrawSelectedModelUvTextureControls(&model, result, true);

                    if (model.hasBounds)
                    {
                        ImGui::Text("Bounds Min (m): %.2f %.2f %.2f", model.minBounds.x, model.minBounds.y, model.minBounds.z);
                        ImGui::Text("Bounds Max (m): %.2f %.2f %.2f", model.maxBounds.x, model.maxBounds.y, model.maxBounds.z);
                    }

                    DrawImportedModelInspector(model);

                    if (!model.sourcePath.empty() &&
                        !model.importedMaterials.empty() &&
                        ImGui::CollapsingHeader("Programmable Material Graph", ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        const char* currentMaterialLabel =
                            model.importedMaterials[static_cast<size_t>(m_materialGraphSelectedIndex)].name.empty()
                            ? "Material"
                            : model.importedMaterials[static_cast<size_t>(m_materialGraphSelectedIndex)].name.c_str();
                        if (ImGui::BeginCombo("Material Slot", currentMaterialLabel))
                        {
                            for (size_t materialIndex = 0; materialIndex < model.importedMaterials.size(); ++materialIndex)
                            {
                                const bool isSelected = static_cast<int>(materialIndex) == m_materialGraphSelectedIndex;
                                const std::string materialLabel =
                                    model.importedMaterials[materialIndex].name.empty()
                                    ? ("Material " + std::to_string(materialIndex))
                                    : model.importedMaterials[materialIndex].name;
                                if (ImGui::Selectable(materialLabel.c_str(), isSelected))
                                {
                                    m_materialGraphSelectedIndex = static_cast<int>(materialIndex);
                                    m_materialGraphDraftModelPath = model.sourcePath;
                                    m_materialGraphDraftIndex = m_materialGraphSelectedIndex;
                                    m_materialGraphDraft = model.importedMaterials[materialIndex];
                                    m_materialGraphDraftDirty = false;
                                }
                                if (isSelected)
                                {
                                    ImGui::SetItemDefaultFocus();
                                }
                            }
                            ImGui::EndCombo();
                        }

                        ImGui::TextWrapped(
                            "This graph edits the imported asset manifest and blends two PBR texture layers directly in the shader."
                        );
                        ImGui::TextDisabled("Drag a node by its colored header.");
                        DrawMaterialGraphLegendChip("A", "Primary Layer", IM_COL32(64, 100, 164, 255), m_effectiveUiScale);
                        DrawMaterialGraphLegendChip("B", "Secondary Layer", IM_COL32(162, 92, 60, 255), m_effectiveUiScale);
                        DrawMaterialGraphLegendChip("MIX", "Mask Blend", IM_COL32(94, 130, 82, 255), m_effectiveUiScale);
                        DrawMaterialGraphLegendChip("PBR", "Shader Output", IM_COL32(124, 96, 56, 255), m_effectiveUiScale);
                        ImGui::NewLine();
                        ImGui::TextDisabled(
                            "Secondary graph inputs: %u",
                            static_cast<unsigned int>(CountMaterialGraphSecondaryTextures(m_materialGraphDraft.blendGraph))
                        );
                        if (!model.baseColorTextureOverridePath.empty())
                        {
                            ImGui::TextWrapped(
                                "Note: this entity still has a base color override, so Layer A base color may be visually overridden here."
                            );
                        }

                        bool graphChanged = false;
                        const ImVec2 logicalCanvasSize(960.0f, 700.0f);
                        const float canvasHeight = 560.0f * m_effectiveUiScale;
                        const ImVec2 virtualCanvasSize(
                            logicalCanvasSize.x * m_effectiveUiScale,
                            logicalCanvasSize.y * m_effectiveUiScale
                        );
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.1f, 0.14f, 1.0f));
                        if (ImGui::BeginChild(
                            "ProgrammablePbrGraphCanvas",
                            ImVec2(0.0f, canvasHeight),
                            true,
                            ImGuiWindowFlags_HorizontalScrollbar
                        ))
                        {
                            const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
                            ImDrawList* canvasDrawList = ImGui::GetWindowDrawList();
                            const ImU32 gridColor = IM_COL32(42, 48, 58, 110);
                            for (float x = 0.0f; x < virtualCanvasSize.x; x += 36.0f * m_effectiveUiScale)
                            {
                                canvasDrawList->AddLine(
                                    ImVec2(canvasOrigin.x + x, canvasOrigin.y),
                                    ImVec2(canvasOrigin.x + x, canvasOrigin.y + virtualCanvasSize.y),
                                    gridColor,
                                    1.0f
                                );
                            }
                            for (float y = 0.0f; y < virtualCanvasSize.y; y += 36.0f * m_effectiveUiScale)
                            {
                                canvasDrawList->AddLine(
                                    ImVec2(canvasOrigin.x, canvasOrigin.y + y),
                                    ImVec2(canvasOrigin.x + virtualCanvasSize.x, canvasOrigin.y + y),
                                    gridColor,
                                    1.0f
                                );
                            }

                            const MaterialGraphNodeFrame layerANode = BeginMaterialGraphNode(
                                "LayerANode",
                                "A",
                                "Layer A",
                                canvasOrigin,
                                m_materialGraphNodeLayout[0],
                                ImVec2(310.0f, 300.0f),
                                logicalCanvasSize,
                                m_effectiveUiScale,
                                IM_COL32(64, 100, 164, 255)
                            );
                            UpdateMaterialGraphNodeDrag(
                                0,
                                layerANode,
                                m_materialGraphNodeLayout[0],
                                ImVec2(310.0f, 300.0f),
                                logicalCanvasSize,
                                m_effectiveUiScale,
                                m_materialGraphDraggingNodeIndex
                            );
                            ImGui::TextDisabled("Primary imported PBR textures");
                            graphChanged |= DrawGraphTextureSlotEditor("Base Color", "LayerA_Base", m_materialGraphDraft.baseColorTexturePath);
                            graphChanged |= DrawGraphTextureSlotEditor("Normal", "LayerA_Normal", m_materialGraphDraft.normalTexturePath);
                            graphChanged |= DrawGraphTextureSlotEditor("Metallic", "LayerA_Metallic", m_materialGraphDraft.metallicTexturePath);
                            graphChanged |= DrawGraphTextureSlotEditor("Roughness", "LayerA_Roughness", m_materialGraphDraft.roughnessTexturePath);
                            graphChanged |= DrawGraphTextureSlotEditor("Occlusion", "LayerA_Occlusion", m_materialGraphDraft.occlusionTexturePath);
                            graphChanged |= DrawGraphTextureSlotEditor("Emissive", "LayerA_Emissive", m_materialGraphDraft.emissiveTexturePath);
                            EndMaterialGraphNode();

                            const MaterialGraphNodeFrame layerBNode = BeginMaterialGraphNode(
                                "LayerBNode",
                                "B",
                                "Layer B",
                                canvasOrigin,
                                m_materialGraphNodeLayout[1],
                                ImVec2(310.0f, 300.0f),
                                logicalCanvasSize,
                                m_effectiveUiScale,
                                IM_COL32(162, 92, 60, 255)
                            );
                            UpdateMaterialGraphNodeDrag(
                                1,
                                layerBNode,
                                m_materialGraphNodeLayout[1],
                                ImVec2(310.0f, 300.0f),
                                logicalCanvasSize,
                                m_effectiveUiScale,
                                m_materialGraphDraggingNodeIndex
                            );
                            ImGui::TextDisabled("Secondary detail textures");
                            graphChanged |= DrawGraphTextureSlotEditor(
                                "Base Color",
                                "LayerB_Base",
                                m_materialGraphDraft.blendGraph.secondaryBaseColorTexturePath
                            );
                            graphChanged |= DrawGraphTextureSlotEditor(
                                "Normal",
                                "LayerB_Normal",
                                m_materialGraphDraft.blendGraph.secondaryNormalTexturePath
                            );
                            graphChanged |= DrawGraphTextureSlotEditor(
                                "Metallic",
                                "LayerB_Metallic",
                                m_materialGraphDraft.blendGraph.secondaryMetallicTexturePath
                            );
                            graphChanged |= DrawGraphTextureSlotEditor(
                                "Roughness",
                                "LayerB_Roughness",
                                m_materialGraphDraft.blendGraph.secondaryRoughnessTexturePath
                            );
                            graphChanged |= DrawGraphTextureSlotEditor(
                                "Occlusion",
                                "LayerB_Occlusion",
                                m_materialGraphDraft.blendGraph.secondaryOcclusionTexturePath
                            );
                            graphChanged |= DrawGraphTextureSlotEditor(
                                "Emissive",
                                "LayerB_Emissive",
                                m_materialGraphDraft.blendGraph.secondaryEmissiveTexturePath
                            );
                            EndMaterialGraphNode();

                            const MaterialGraphNodeFrame blendNode = BeginMaterialGraphNode(
                                "BlendNode",
                                "MIX",
                                "Blend",
                                canvasOrigin,
                                m_materialGraphNodeLayout[2],
                                ImVec2(250.0f, 215.0f),
                                logicalCanvasSize,
                                m_effectiveUiScale,
                                IM_COL32(94, 130, 82, 255)
                            );
                            UpdateMaterialGraphNodeDrag(
                                2,
                                blendNode,
                                m_materialGraphNodeLayout[2],
                                ImVec2(250.0f, 215.0f),
                                logicalCanvasSize,
                                m_effectiveUiScale,
                                m_materialGraphDraggingNodeIndex
                            );
                            graphChanged |= ImGui::Checkbox("Enable Shader Graph Blend", &m_materialGraphDraft.blendGraph.enabled);
                            graphChanged |= ImGui::SliderFloat(
                                "Blend Factor",
                                &m_materialGraphDraft.blendGraph.blendFactor,
                                0.0f,
                                1.0f,
                                "%.2f"
                            );
                            graphChanged |= DrawGraphTextureSlotEditor(
                                "Blend Mask",
                                "Blend_Mask",
                                m_materialGraphDraft.blendGraph.blendMaskTexturePath
                            );
                            ImGui::ProgressBar(
                                m_materialGraphDraft.blendGraph.enabled ? m_materialGraphDraft.blendGraph.blendFactor : 0.0f,
                                ImVec2(-FLT_MIN, 0.0f),
                                m_materialGraphDraft.blendGraph.enabled ? "Shader blend active" : "Shader blend disabled"
                            );
                            ImGui::TextWrapped("Final weight = Blend Factor x Mask.r");
                            EndMaterialGraphNode();

                            const MaterialGraphNodeFrame outputNode = BeginMaterialGraphNode(
                                "OutputNode",
                                "PBR",
                                "Output",
                                canvasOrigin,
                                m_materialGraphNodeLayout[3],
                                ImVec2(220.0f, 185.0f),
                                logicalCanvasSize,
                                m_effectiveUiScale,
                                IM_COL32(124, 96, 56, 255)
                            );
                            UpdateMaterialGraphNodeDrag(
                                3,
                                outputNode,
                                m_materialGraphNodeLayout[3],
                                ImVec2(220.0f, 185.0f),
                                logicalCanvasSize,
                                m_effectiveUiScale,
                                m_materialGraphDraggingNodeIndex
                            );
                            ImGui::TextWrapped("Blend state: %s", m_materialGraphDraft.blendGraph.enabled ? "Enabled" : "Disabled");
                            ImGui::Text("Primary: 6 PBR inputs");
                            ImGui::Text(
                                "Secondary: %u bound",
                                static_cast<unsigned int>(CountMaterialGraphSecondaryTextures(m_materialGraphDraft.blendGraph))
                            );
                            ImGui::TextWrapped("Writes to asset manifest and rebuilds renderables.");
                            ImGui::BeginDisabled(!m_materialGraphDraftDirty);
                            if (ImGui::Button("Apply Graph"))
                            {
                                result.actions.updatedImportedMaterial = EditorUiActions::ImportedMaterialUpdate{
                                    model.sourcePath,
                                    static_cast<uint32_t>(m_materialGraphSelectedIndex),
                                    m_materialGraphDraft
                                };
                                m_materialGraphDraftDirty = false;
                            }
                            ImGui::EndDisabled();
                            ImGui::SameLine();
                            if (ImGui::Button("Reset Draft"))
                            {
                                m_materialGraphDraft =
                                    model.importedMaterials[static_cast<size_t>(m_materialGraphSelectedIndex)];
                                m_materialGraphDraftDirty = false;
                            }
                            if (ImGui::Button("Reset Layout"))
                            {
                                m_materialGraphNodeLayout = BuildDefaultMaterialGraphNodeLayout();
                            }
                            EndMaterialGraphNode();

                            DrawMaterialGraphLink(
                                canvasDrawList,
                                layerANode.outputPin,
                                blendNode.inputPin,
                                IM_COL32(112, 164, 255, 255),
                                m_effectiveUiScale
                            );
                            DrawMaterialGraphLink(
                                canvasDrawList,
                                layerBNode.outputPin,
                                blendNode.inputPinAlt,
                                IM_COL32(236, 142, 96, 255),
                                m_effectiveUiScale
                            );
                            DrawMaterialGraphLink(
                                canvasDrawList,
                                blendNode.outputPin,
                                outputNode.inputPin,
                                IM_COL32(164, 220, 168, 255),
                                m_effectiveUiScale
                            );

                            ImGui::SetCursorScreenPos(
                                ImVec2(
                                    canvasOrigin.x + virtualCanvasSize.x - 1.0f,
                                    canvasOrigin.y + virtualCanvasSize.y - 1.0f
                                )
                            );
                            ImGui::Dummy(ImVec2(1.0f, 1.0f));
                        }
                        ImGui::EndChild();
                        ImGui::PopStyleColor();

                        if (graphChanged)
                        {
                            m_materialGraphDraftDirty = true;
                        }
                        ImGui::TextDisabled("Draft: %s", m_materialGraphDraftDirty ? "modified" : "in sync");
                    }
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

    return result;
}

void EditorUiController::ApplyUiScale()
{
    ImGuiStyle& style = ImGui::GetStyle();
    m_effectiveUiScale = std::clamp(GetWindowUiScale() * m_uiScale, 0.75f, 3.0f);

    if (std::abs(style.FontScaleMain - m_effectiveUiScale) <= 0.001f)
    {
        return;
    }

    style = m_baseStyle;
    style.ScaleAllSizes(m_effectiveUiScale);
    style.FontScaleMain = m_effectiveUiScale;
}

float EditorUiController::GetWindowUiScale() const
{
    if (m_window == nullptr)
    {
        return 1.0f;
    }

    const float displayScale = SDL_GetWindowDisplayScale(m_window);
    if (displayScale > 0.0f)
    {
        return displayScale;
    }

    const float pixelDensity = SDL_GetWindowPixelDensity(m_window);
    return pixelDensity > 0.0f ? pixelDensity : 1.0f;
}
