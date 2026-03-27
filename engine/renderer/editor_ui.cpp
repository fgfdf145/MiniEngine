#include "editor_ui.h"

#include <editor_scene.h>
#include <file_dialog/file_dialog.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <glm/common.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <system_error>

namespace
{
constexpr ImU32 kSelectionOutlineColor = IM_COL32(255, 196, 64, 255);
constexpr float kSelectionCenterHitRadiusPixels = 20.0f;
constexpr float kSelectionBoundsHitPaddingPixels = 6.0f;
constexpr float kSelectionOutlineThickness = 2.0f;
constexpr ImGuiDockNodeFlags kEditorDockspaceFlags = ImGuiDockNodeFlags_PassthruCentralNode;

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

void EnsureDefaultDockLayout(ImGuiID dockspaceId)
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
    ImGui::DockBuilderSetNodeSize(dockspaceId, mainViewport->Size);

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

std::string DetectImporterName(const ModelComponent& model)
{
    if (model.sourcePath.empty())
    {
        return "Built-in";
    }

    const std::string extension = ToLowerCopy(std::filesystem::path(model.sourcePath).extension().string());
    return extension == ".fbx" ? "FBX SDK" : "Assimp";
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

void DrawImportedModelInspector(const ModelComponent& model)
{
    if (model.importedSubmeshes.empty() && model.importedMaterials.empty())
    {
        return;
    }

    ImGui::Separator();
    ImGui::Text("Importer: %s", DetectImporterName(model).c_str());
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
            const std::string materialName = material.name.empty()
                ? ("Material " + std::to_string(materialIndex))
                : material.name;
            const std::string treeLabel = materialName + "##material_" + std::to_string(materialIndex);
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

bool IsSupportedModelAssetPath(const std::filesystem::path& path)
{
    static constexpr std::array<const char*, 8> kExtensions = {
        ".obj", ".fbx", ".gltf", ".glb", ".dae", ".3ds", ".ply", ".stl"
    };

    const std::string extension = ToLowerCopy(path.extension().string());
    return std::find(kExtensions.begin(), kExtensions.end(), extension) != kExtensions.end();
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
        entries.push_back(*iterator);
    }

    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right)
    {
        const bool leftIsDirectory = left.is_directory();
        const bool rightIsDirectory = right.is_directory();
        if (leftIsDirectory != rightIsDirectory)
        {
            return leftIsDirectory > rightIsDirectory;
        }

        return ToLowerCopy(left.path().filename().string()) < ToLowerCopy(right.path().filename().string());
    });
}

void DrawAssetTreeNode(
    const std::filesystem::path& path,
    EditorUiFrameResult& result,
    std::string& selectedAssetPath,
    bool& selectedAssetIsDirectory,
    bool defaultOpen = false
)
{
    const std::string normalizedPath = path.lexically_normal().string();
    const std::string nodeLabel = path.filename().empty() ? path.string() : path.filename().string();
    std::error_code errorCode;
    const bool isDirectory = std::filesystem::is_directory(path, errorCode) && !errorCode;
    const bool isSelected = selectedAssetPath == normalizedPath;

    if (!isDirectory)
    {
        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_Leaf |
            ImGuiTreeNodeFlags_NoTreePushOnOpen |
            ImGuiTreeNodeFlags_SpanAvailWidth;
        if (isSelected)
        {
            flags |= ImGuiTreeNodeFlags_Selected;
        }

        ImGui::TreeNodeEx(normalizedPath.c_str(), flags, "%s", nodeLabel.c_str());
        if (ImGui::IsItemClicked())
        {
            selectedAssetPath = normalizedPath;
            selectedAssetIsDirectory = false;
        }
        if (ImGui::IsItemHovered() &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            IsSupportedModelAssetPath(path))
        {
            result.actions.selectedModelPath = normalizedPath;
        }
        return;
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (isSelected)
    {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (defaultOpen)
    {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }

    const bool open = ImGui::TreeNodeEx(normalizedPath.c_str(), flags, "%s", nodeLabel.c_str());
    if (ImGui::IsItemClicked())
    {
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
        DrawAssetTreeNode(entry.path(), result, selectedAssetPath, selectedAssetIsDirectory);
    }
    ImGui::TreePop();
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

    const ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), kEditorDockspaceFlags);
    EnsureDefaultDockLayout(dockspaceId);

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
            if (ImGui::Button("Import Model"))
            {
                result.actions.importedModelSourcePath = OpenModelFileDialog();
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

            const std::filesystem::path assetRoot = std::filesystem::path(MINIENGINE_ASSET_DIR);
            std::error_code assetErrorCode;
            std::filesystem::create_directories(assetRoot, assetErrorCode);

            ImGui::Separator();
            ImGui::TextWrapped("Asset Root: %s", assetRoot.string().c_str());
            ImGui::TextWrapped("Current Model: %s", currentModelPath.empty() ? "<builtin cube>" : currentModelPath.c_str());
            ImGui::TextWrapped("Current Scene: %s", scene.GetSceneFilePath().empty() ? "<unsaved>" : scene.GetSceneFilePath().c_str());

            const float treeHeight = std::max(ImGui::GetContentRegionAvail().y * 0.5f, 220.0f);
            if (ImGui::BeginChild("AssetTree", ImVec2(0.0f, treeHeight), true))
            {
                DrawAssetTreeNode(assetRoot, result, m_selectedAssetPath, m_selectedAssetIsDirectory, true);
            }
            ImGui::EndChild();

            ImGui::SeparatorText("Selected Asset");
            if (m_selectedAssetPath.empty())
            {
                ImGui::TextUnformatted("No asset selected.");
            }
            else
            {
                const std::filesystem::path selectedAssetPath(m_selectedAssetPath);
                ImGui::TextWrapped("Path: %s", m_selectedAssetPath.c_str());
                ImGui::Text("Type: %s", m_selectedAssetIsDirectory ? "Folder" : "File");

                const bool canLoadSelectedModel =
                    scene.HasSelection() &&
                    !m_selectedAssetIsDirectory &&
                    IsSupportedModelAssetPath(selectedAssetPath);
                ImGui::BeginDisabled(!canLoadSelectedModel);
                if (ImGui::Button("Load Selected Model"))
                {
                    result.actions.selectedModelPath = m_selectedAssetPath;
                }
                ImGui::EndDisabled();

                ImGui::SameLine();

                const bool canDeleteSelectedAsset = selectedAssetPath != assetRoot;
                ImGui::BeginDisabled(!canDeleteSelectedAsset);
                if (ImGui::Button("Delete Selected"))
                {
                    result.actions.deleteAssetPath = m_selectedAssetPath;
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
            ImGui::TextUnformatted("F to frame, W/E/R to switch gizmo");
            ImGui::Text("Render Size: %u x %u", result.viewportExtent.width, result.viewportExtent.height);
            ImGui::EndGroup();
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    return result;
}

void EditorUiController::ApplyUiScale()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGuiStyle& style = ImGui::GetStyle();
    m_effectiveUiScale = std::clamp(GetWindowUiScale() * m_uiScale, 0.75f, 3.0f);

    if (std::abs(io.FontGlobalScale - m_effectiveUiScale) <= 0.001f)
    {
        return;
    }

    style = m_baseStyle;
    style.ScaleAllSizes(m_effectiveUiScale);
    io.FontGlobalScale = m_effectiveUiScale;
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
