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
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <limits>

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
    ImGui::DockBuilderDockWindow("Assets", upperRightNode);
    ImGui::DockBuilderDockWindow("Camera", lowerRightNode);
    ImGui::DockBuilderDockWindow("Renderer", lowerRightNode);
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

void DrawRendererWindow(RenderBackendType currentBackendType, EditorUiFrameResult& result)
{
    ImGui::Begin("Renderer");
    ImGui::Text("Current Backend: %s", ToString(currentBackendType));
    ImGui::Separator();

    const bool usingVulkan = currentBackendType == RenderBackendType::Vulkan;
    ImGui::BeginDisabled(usingVulkan);
    if (ImGui::Button("Switch To Vulkan"))
    {
        result.actions.requestedBackendType = RenderBackendType::Vulkan;
    }
    ImGui::EndDisabled();

    const bool usingOpenGL = currentBackendType == RenderBackendType::OpenGL;
    ImGui::BeginDisabled(usingOpenGL);
    if (ImGui::Button("Switch To OpenGL"))
    {
        result.actions.requestedBackendType = RenderBackendType::OpenGL;
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextWrapped("Switching backend rebuilds the renderer and recreates the graphics context on the next frame.");
    ImGui::End();
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

    ImGui::Begin("Camera");
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
    ImGui::End();

    ImGui::Begin("Assets");
    if (ImGui::Button("Load Model"))
    {
        result.actions.selectedModelPath = OpenModelFileDialog();
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

    ImGui::Separator();
    ImGui::TextWrapped("Current Model: %s", currentModelPath.empty() ? "<builtin cube>" : currentModelPath.c_str());
    ImGui::TextWrapped("Current Scene: %s", scene.GetSceneFilePath().empty() ? "<unsaved>" : scene.GetSceneFilePath().c_str());
    if (!lastLoadError.empty())
    {
        ImGui::TextWrapped("Last Error: %s", lastLoadError.c_str());
    }
    if (!lastSceneIoError.empty())
    {
        ImGui::TextWrapped("Scene Error: %s", lastSceneIoError.c_str());
    }
    ImGui::End();

    ImGui::Begin("Scene");
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
            if (model.hasBounds)
            {
                ImGui::Text("Bounds Min (m): %.2f %.2f %.2f", model.minBounds.x, model.minBounds.y, model.minBounds.z);
                ImGui::Text("Bounds Max (m): %.2f %.2f %.2f", model.maxBounds.x, model.maxBounds.y, model.maxBounds.z);
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
    ImGui::End();

    DrawRendererWindow(currentBackendType, result);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("Viewport");
    const bool flipViewportImageY = currentBackendType == RenderBackendType::OpenGL;
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
    ImGui::End();
    ImGui::PopStyleVar();

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
