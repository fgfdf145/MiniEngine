#include <engine/editor/editor_ui.h>
#include "editor_ui_internal.h"
#include "editor_material_graph.h"
#include "editor_model_preview.h"

#include <engine/asset/material_graph_runtime.h>
#include <engine/asset/material_definition.h>
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
std::string BuildMaterialSlotLabel(const ModelImportedMaterialInfo& material, size_t materialIndex)
{
    return material.name.empty()
               ? ("Material " + std::to_string(materialIndex))
               : material.name;
}

std::vector<ModelImportedMaterialInfo> LoadEffectiveImportedModelMaterials(
    const std::filesystem::path& modelPath,
    const LoadedModelData& loadedModel,
    std::vector<std::string>* materialAssetPaths)
{
    static_cast<void>(modelPath);
    std::vector<ModelImportedMaterialInfo> materials;
    materials.reserve(loadedModel.materials.size());
    for (const ModelMaterialData& material : loadedModel.materials)
    {
        ModelImportedMaterialInfo importedMaterial = BuildImportedMaterialInfo(material);
        EnsureMaterialShaderGraph(importedMaterial.name, std::nullopt, importedMaterial);
        CompileMaterialShaderGraph(importedMaterial);
        materials.push_back(std::move(importedMaterial));
    }

    if (materials.empty())
    {
        materials.push_back(ModelImportedMaterialInfo{});
    }

    if (materialAssetPaths != nullptr)
    {
        materialAssetPaths->assign(materials.size(), std::string{});
    }
    return materials;
}
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

    try
    {
        const LoadedModelData loadedModel = ModelLoader::LoadModel(m_modelProcessorModelPath);
        m_modelProcessorLoadedModel = loadedModel;
        m_modelProcessorMaterials =
            LoadEffectiveImportedModelMaterials(normalizedPath, loadedModel, nullptr);
    }
    catch (const std::exception& error)
    {
        m_modelProcessorStatusMessage = error.what();
    }
}

void EditorUiController::CloseModelProcessorWindow()
{
    ResetMaterialShadedPreviewCache("ModelDraftPreviewCanvas");
    m_showModelProcessorWindow = false;
    m_modelProcessorModelPath.clear();
    m_modelProcessorDisplayName.clear();
    m_modelProcessorStatusMessage.clear();
    m_modelProcessorMaterials.clear();
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

void EditorUiController::DrawModelProcessorPanel(IEditorWorld& scene, EditorUiFrameResult& result)
{
    bool keepModelProcessorWindowOpen = m_showModelProcessorWindow;
    bool requestCloseModelProcessorWindow = false;
    bool requestReloadModelProcessorWindow = false;
    const std::string modelProcessorReloadPath = m_modelProcessorModelPath;

    ImGui::SetNextWindowSize(
        ImVec2(560.0f * m_effectiveUiScale, 560.0f * m_effectiveUiScale),
        ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Model Preview", &keepModelProcessorWindowOpen))
    {
        ImGui::TextWrapped("Model: %s", m_modelProcessorDisplayName.empty() ? "<unknown>" : m_modelProcessorDisplayName.c_str());
        ImGui::TextWrapped("Asset Path: %s", m_modelProcessorModelPath.c_str());
        ImGui::TextWrapped(
            "Scene Target: %s",
            scene.HasSelection() ? scene.GetSelectedTag().name.c_str() : "<no entity selected>");
        ImGui::TextDisabled("Asset management is disabled while it is being rebuilt.");

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
                "Approximate PBR draft preview");
            ImGui::Text(
                "Submeshes: %u  Triangles: %u",
                static_cast<unsigned int>(m_modelProcessorLoadedModel.submeshes.size()),
                static_cast<unsigned int>(previewTriangleCount));

            ImGui::Spacing();
            ImGui::SeparatorText("UV Preview");
            DrawModelUvPreview(
                m_modelProcessorLoadedModel,
                m_modelProcessorSelectedUvSubmeshIndex,
                m_effectiveUiScale);
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
                static_cast<int>(m_modelProcessorMaterials.size()) - 1);

            const auto buildCurrentSlotLabel = [&]() -> std::string
            {
                return BuildMaterialSlotLabel(
                    m_modelProcessorMaterials[static_cast<size_t>(m_modelProcessorSelectedMaterialIndex)],
                    static_cast<size_t>(m_modelProcessorSelectedMaterialIndex));
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
            ImGui::Text("Draft State: %s", m_modelProcessorDirty ? "Modified" : "Clean");

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
                selectedGraphNodeForActions->type == MaterialShaderNodeType::Output);
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
                    m_materialGraphLinkDragFromSlot.c_str());
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
                    GetMaterialGraphNodeTypeLabel(selectedGraphNode->type));
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
                    selectedGraphLink->toSlot.c_str());
            }
            else
            {
                ImGui::TextDisabled("Tip: right-click the graph background or use Add Node to expand the material graph.");
            }

            if (ImGui::BeginChild(
                    "MaterialShaderGraphCanvas",
                    ImVec2(0.0f, 660.0f * m_effectiveUiScale),
                    true,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
            {
                ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
                ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
                ImGui::SetItemKeyOwner(ImGuiKey_MouseMiddle);

                const ImVec2 canvasOrigin =
                    ImVec2(
                        ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x,
                        ImGui::GetWindowPos().y + ImGui::GetWindowContentRegionMin().y);
                const ImVec2 canvasMax =
                    ImVec2(
                        ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x,
                        ImGui::GetWindowPos().y + ImGui::GetWindowContentRegionMax().y);
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
                    m_materialGraphZoom);
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
                        ImGuiInputFlags_LockThisFrame);
                    ImGui::SetKeyOwner(
                        ImGuiKey_MouseWheelX,
                        canvasInputOwner,
                        ImGuiInputFlags_LockThisFrame);
                    ImGui::SetKeyOwner(
                        ImGuiKey_MouseMiddle,
                        canvasInputOwner,
                        ImGuiInputFlags_LockUntilRelease);
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
                            m_materialGraphZoom);
                    const float zoomFactor = std::pow(kMaterialGraphZoomStep, ImGui::GetIO().MouseWheel);
                    m_materialGraphZoom = std::clamp(
                        m_materialGraphZoom * zoomFactor,
                        kMaterialGraphMinZoom,
                        kMaterialGraphMaxZoom);
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
                        m_materialGraphViewOrigin.y + visibleHeight * 0.22f / m_materialGraphZoom};
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
                        m_materialGraphZoom);
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
                        1.0f);
                }
                const float gridOffsetY =
                    std::fmod(-(m_materialGraphViewOrigin.y * m_materialGraphZoom), gridStep);
                for (float y = gridOffsetY; y < canvasMax.y - canvasOrigin.y; y += gridStep)
                {
                    graphDrawList->AddLine(
                        ImVec2(canvasOrigin.x, canvasOrigin.y + y),
                        ImVec2(canvasMax.x, canvasOrigin.y + y),
                        IM_COL32(44, 54, 70, 90),
                        1.0f);
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
                            ImGui::GetIO().MousePos.y - m_materialGraphResizeStartMouse.y);
                        materialChanged |= ApplyMaterialGraphNodeResize(
                            *resizingNode,
                            m_materialGraphResizeEdges,
                            m_materialGraphResizeStartPosition,
                            m_materialGraphResizeStartSize,
                            resizeMouseDelta,
                            m_effectiveUiScale,
                            m_materialGraphZoom);
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
                        &m_modelProcessorStatusMessage);
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
                        drawResult.pins.end());
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
                            : 2.6f * nodeUiScale);
                }

                if (m_materialGraphLinkDragActive)
                {
                    const MaterialGraphRenderedPin* dragFromPin = FindRenderedMaterialGraphPin(
                        renderedPins,
                        m_materialGraphLinkDragFromNodeId,
                        m_materialGraphLinkDragFromSlot,
                        false);
                    if (dragFromPin != nullptr)
                    {
                        DrawNodeConnection(
                            graphDrawList,
                            dragFromPin->center,
                            ImGui::GetIO().MousePos,
                            kSelectionOutlineColor,
                            2.4f * nodeUiScale);
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
                            *pendingPasteNodePosition);
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
                                m_materialGraphContextSpawnPosition);
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
                            *pendingPasteNodePosition);
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
                selectedMaterial.pbr.opacity);
            ImGui::Text("Alpha Mode: %s", ToString(selectedMaterial.pbr.alphaMode));
            if (selectedMaterial.pbr.alphaMode == MaterialAlphaMode::Mask)
            {
                ImGui::Text("Alpha Cutoff: %.2f", selectedMaterial.pbr.alphaCutoff);
            }
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
                    m_modelProcessorMaterials};
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
}
