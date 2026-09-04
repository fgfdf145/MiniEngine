#include <engine/editor/editor_ui.h>
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
constexpr float kSelectionCenterHitRadiusPixels = 20.0f;
constexpr float kSelectionBoundsHitPaddingPixels = 6.0f;
constexpr float kSelectionOutlineThickness = 2.0f;

struct ProjectedEntityCenter
{
    entt::entity entity = entt::null;
    ImVec2 center{0.0f, 0.0f};
    ImVec2 min{0.0f, 0.0f};
    ImVec2 max{0.0f, 0.0f};
    float depth = std::numeric_limits<float>::max();
};

struct ViewportOverlayRect
{
    ImVec2 origin{0.0f, 0.0f};
    ImVec2 size{0.0f, 0.0f};
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
        std::max(static_cast<uint32_t>(std::lround(rect.size.y)), 1u)};
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
        glm::vec3(maxBounds.x, maxBounds.y, maxBounds.z)};
}

constexpr std::array<std::pair<size_t, size_t>, 12> kBoundsEdges = {{{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}}};

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

bool ProjectWorldPointToViewport(
    const glm::vec3& worldPoint,
    const glm::mat4& viewProjection,
    const ViewportOverlayRect& viewportRect,
    ImVec2& projectedPoint)
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
        viewportRect.origin.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * viewportRect.size.y);
    return true;
}

bool BuildProjectedSelectionBox(
    const IEditorWorld& scene,
    entt::entity entity,
    const ViewportMatrices& matrices,
    const ViewportOverlayRect& viewportRect,
    std::array<ImVec2, 8>& projectedCorners)
{
    const ModelBoundsComponent& bounds = scene.GetModelBounds(entity);
    if (!bounds.hasBounds || viewportRect.size.x <= 0.0f || viewportRect.size.y <= 0.0f)
    {
        return false;
    }

    const glm::mat4 modelMatrix = scene.GetModelMatrix(entity);
    const glm::mat4 viewProjection = matrices.projection * matrices.view;
    const auto localCorners = BuildBoundsCorners(bounds.minBounds, bounds.maxBounds);
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
    glm::vec3& maxBounds)
{
    const ModelBoundsComponent& bounds = scene.GetModelBounds(entity);
    if (!bounds.hasBounds)
    {
        return false;
    }

    minBounds = glm::vec3(std::numeric_limits<float>::max());
    maxBounds = glm::vec3(std::numeric_limits<float>::lowest());
    const glm::mat4 modelMatrix = scene.GetModelMatrix(entity);
    for (const glm::vec3& corner : BuildBoundsCorners(bounds.minBounds, bounds.maxBounds))
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
    const ViewportOverlayRect& viewportRect)
{
    std::vector<ProjectedEntityCenter> projectedCenters;
    if (viewportRect.size.x <= 0.0f || viewportRect.size.y <= 0.0f)
    {
        return projectedCenters;
    }

    const glm::mat4 viewProjection = matrices.projection * matrices.view;
    projectedCenters.reserve(scene.Registry().view<const ModelComponent>().size());

    for (entt::entity entity : scene.Registry().view<const ModelBoundsComponent>())
    {
        const ModelBoundsComponent& bounds = scene.GetModelBounds(entity);
        if (!bounds.hasBounds)
        {
            continue;
        }

        ProjectedEntityCenter projected{};
        projected.entity = entity;
        std::array<ImVec2, 8> projectedCorners{};
        if (!BuildProjectedSelectionBox(scene, entity, matrices, viewportRect, projectedCorners))
        {
            continue;
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
            continue;
        }

        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f || ndc.z < 0.0f || ndc.z > 1.0f)
        {
            continue;
        }

        projected.center = ImVec2(
            viewportRect.origin.x + (ndc.x * 0.5f + 0.5f) * viewportRect.size.x,
            viewportRect.origin.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * viewportRect.size.y);
        projected.depth = ndc.z;
        projectedCenters.push_back(projected);
    }

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

void DrawLightViewportIcon(
    ImDrawList* drawList,
    const ImVec2& screenPos,
    LightType type,
    bool selected,
    float scale)
{
    const ImU32 typeColor = GetLightTypeColor(type);
    const float radius = 10.0f * scale;
    const ImU32 fillColor = selected
                                ? IM_COL32(255, 196, 64, 220)
                                : IM_COL32(255, 255, 255, 80);

    drawList->AddCircleFilled(screenPos, radius, fillColor);
    drawList->AddCircle(screenPos, radius, typeColor, 16, selected ? 2.5f : 1.5f);

    const char* badge = "L";
    if (type == LightType::Directional)
        badge = "D";
    else if (type == LightType::Spot)
        badge = "S";
    else if (type == LightType::Area)
        badge = "A";
    else if (type == LightType::Ambient)
        badge = "*";

    const ImVec2 textSize = ImGui::CalcTextSize(badge);
    drawList->AddText(
        ImVec2(screenPos.x - textSize.x * 0.5f, screenPos.y - textSize.y * 0.5f),
        typeColor,
        badge);
}

void DrawLightSelectionIndicator(
    const IEditorWorld& scene,
    entt::entity entity,
    const ViewportMatrices& matrices,
    const ViewportOverlayRect& viewportRect)
{
    if (viewportRect.drawList == nullptr)
        return;

    const glm::mat4 modelMat = scene.GetModelMatrix(entity);
    const glm::vec3 worldPos = glm::vec3(modelMat[3]);
    const glm::mat4 vp = matrices.projection * matrices.view;

    ImVec2 screenPos;
    if (!ProjectWorldPointToViewport(worldPos, vp, viewportRect, screenPos))
        return;

    const LightComponent& light = scene.GetLightComponent(entity);
    viewportRect.drawList->AddCircle(screenPos, 14.0f, kSelectionOutlineColor, 24, kSelectionOutlineThickness);
    DrawLightViewportIcon(viewportRect.drawList, screenPos, light.type, true, 1.0f);
}

void DrawViewportSelectionOverlay(
    const IEditorWorld& scene,
    const ViewportMatrices& matrices,
    const ViewportOverlayRect& viewportRect,
    const std::vector<ProjectedEntityCenter>& projectedCenters)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (drawList == nullptr || !scene.HasSelection())
    {
        return;
    }

    const entt::entity selected = scene.GetSelectedEntity();

    // Light entity: draw icon highlight instead of bounds box
    if (scene.HasLightComponent(selected))
    {
        DrawLightSelectionIndicator(scene, selected, matrices, viewportRect);
        return;
    }

    std::array<ImVec2, 8> projectedSelectionCorners{};
    if (!BuildProjectedSelectionBox(scene, selected, matrices, viewportRect, projectedSelectionCorners))
    {
        return;
    }

    for (const auto& edge : kBoundsEdges)
    {
        drawList->AddLine(
            projectedSelectionCorners[edge.first],
            projectedSelectionCorners[edge.second],
            kSelectionOutlineColor,
            kSelectionOutlineThickness);
    }

    for (const ImVec2& corner : projectedSelectionCorners)
    {
        drawList->AddCircleFilled(corner, 2.5f, kSelectionOutlineColor);
    }
}

void RefreshViewportMatrices(
    Camera& camera,
    ViewportMatrices& matrices,
    const IEditorWorld& scene,
    RenderExtent viewportExtent,
    RenderBackendType currentBackendType)
{
    const bool useZeroToOneDepth = UsesZeroToOneDepth(currentBackendType);
    const bool invertRenderYAxis = UsesInvertedRenderYAxis(currentBackendType);
    matrices.view = camera.GetViewMatrix();
    matrices.projection = camera.GetProjectionMatrix(viewportExtent, false, useZeroToOneDepth);
    matrices.renderProjection = camera.GetProjectionMatrix(viewportExtent, invertRenderYAxis, useZeroToOneDepth);
    matrices.model =
        scene.HasSelection() ? scene.GetModelMatrix(scene.GetSelectedEntity()) : glm::mat4(1.0f);
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
    if (ImGui::IsKeyPressed(ImGuiKey_R, false) && !ImGuizmo::IsUsing())
    {
        gizmo.operation = ToggleGizmoOperation(gizmo.operation);
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
    const ViewportOverlayRect& viewportRect)
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
    const glm::mat4 viewBefore = matrices.view;
    ImGuizmo::ViewManipulate(
        glm::value_ptr(matrices.view),
        7.5f,
        ImVec2(viewportRect.origin.x + viewportRect.size.x - 144.0f, viewportRect.origin.y + 16.0f),
        ImVec2(128.0f, 128.0f),
        IM_COL32(32, 32, 32, 180));
    if (matrices.view != viewBefore)
    {
        camera.SetFromViewMatrix(matrices.view);
        matrices.view = camera.GetViewMatrix();
    }
}

void DrawGizmoOverlay(
    IEditorWorld& scene,
    ViewportMatrices& matrices,
    const ViewportOverlayRect& viewportRect,
    GizmoDragSnapState& dragSnapState)
{
    if (!scene.HasSelection() ||
        viewportRect.size.x <= 0.0f ||
        viewportRect.size.y <= 0.0f ||
        viewportRect.drawList == nullptr)
    {
        dragSnapState.FinishManipulate(false);
        return;
    }

    entt::entity selectedEntity = scene.GetSelectedEntity();
    GizmoSettings& gizmo = scene.GetGizmoSettings();
    matrices.model = scene.GetModelMatrix(selectedEntity);
    const glm::vec3 localCenter = scene.GetBoundsCenter(selectedEntity);
    const glm::mat4 pivotOffset = glm::translate(glm::mat4(1.0f), localCenter);
    const glm::mat4 inversePivotOffset = glm::translate(glm::mat4(1.0f), -localCenter);
    glm::mat4 gizmoMatrix = matrices.model * pivotOffset;

    // Point and Ambient lights have no meaningful orientation — restrict to translate
    // without mutating gizmo.operation so the user's preference is preserved for other entities.
    const LightComponent* selectedLight = scene.Registry().try_get<LightComponent>(selectedEntity);
    const bool isTranslateOnly =
        selectedLight != nullptr &&
        (selectedLight->type == LightType::Point || selectedLight->type == LightType::Ambient);
    const ImGuizmo::OPERATION effectiveOperation = isTranslateOnly ? ImGuizmo::TRANSLATE : gizmo.operation;

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetID(static_cast<int>(entt::to_integral(selectedEntity)));
    ImGuizmo::SetDrawlist(viewportRect.drawList);
    ImGuizmo::SetRect(viewportRect.origin.x, viewportRect.origin.y, viewportRect.size.x, viewportRect.size.y);

    const bool gizmoWasUsing = ImGuizmo::IsUsing();
    const bool translationHandleHovered =
        !gizmoWasUsing &&
        effectiveOperation != ImGuizmo::SCALE &&
        ImGuizmo::IsOver(ImGuizmo::TRANSLATE);
    const bool rotationHandleHovered =
        !gizmoWasUsing &&
        effectiveOperation != ImGuizmo::SCALE &&
        ImGuizmo::IsOver(ImGuizmo::ROTATE);
    dragSnapState.PrepareForManipulate(
        effectiveOperation,
        gizmoWasUsing,
        translationHandleHovered,
        rotationHandleHovered);
    const std::array<float, 3> snapValues =
        BuildGizmoSnapValues(gizmo, dragSnapState.Family());

    ImGuizmo::Manipulate(
        glm::value_ptr(matrices.view),
        glm::value_ptr(matrices.projection),
        effectiveOperation,
        gizmo.mode,
        glm::value_ptr(gizmoMatrix),
        nullptr,
        gizmo.useSnap ? snapValues.data() : nullptr);

    const bool gizmoIsUsing = ImGuizmo::IsUsing();
    dragSnapState.FinishManipulate(gizmoIsUsing);
    if (!gizmoIsUsing)
    {
        return;
    }

    matrices.model = gizmoMatrix * inversePivotOffset;
    scene.ApplyTransformMatrix(selectedEntity, matrices.model);
}

// ---- Light scene gizmos and viewport helpers ------------------------------

// Project a light's world icon position and optionally add to projected center list.
bool ProjectLightCenter(
    entt::entity entity,
    const glm::vec3& worldPos,
    const glm::mat4& viewProjection,
    const ViewportOverlayRect& viewportRect,
    ProjectedEntityCenter& out)
{
    ImVec2 screenPos;
    if (!ProjectWorldPointToViewport(worldPos, viewProjection, viewportRect, screenPos))
    {
        return false;
    }

    constexpr float kIconHalfSize = 16.0f;
    out.entity = entity;
    out.center = screenPos;
    out.min = ImVec2(screenPos.x - kIconHalfSize, screenPos.y - kIconHalfSize);
    out.max = ImVec2(screenPos.x + kIconHalfSize, screenPos.y + kIconHalfSize);

    // Compute NDC depth for depth-sorting with model entities.
    const glm::vec4 clip = viewProjection * glm::vec4(worldPos, 1.0f);
    out.depth = (clip.w > 0.0f) ? (clip.z / clip.w) : 1.0f;
    return true;
}

// Draw a wireframe sphere (3 great-circle rings) for point lights.
void DrawLightSphereGizmo(
    ImDrawList* drawList,
    const glm::vec3& center,
    float radius,
    const glm::mat4& viewProjection,
    const ViewportOverlayRect& viewportRect,
    ImU32 color)
{
    constexpr int kSegments = 24;
    const auto project = [&](glm::vec3 p) -> std::optional<ImVec2>
    {
        ImVec2 s;
        if (!ProjectWorldPointToViewport(p, viewProjection, viewportRect, s))
            return std::nullopt;
        return s;
    };

    // Three orthogonal rings (XZ, XY, YZ planes)
    const glm::vec3 axes[3][2] = {
        {{1, 0, 0}, {0, 0, 1}},
        {{1, 0, 0}, {0, 1, 0}},
        {{0, 1, 0}, {0, 0, 1}}};
    for (auto& ax : axes)
    {
        for (int i = 0; i < kSegments; ++i)
        {
            const float a0 = (static_cast<float>(i) / kSegments) * 2.0f * 3.14159265f;
            const float a1 = (static_cast<float>(i + 1) / kSegments) * 2.0f * 3.14159265f;
            const auto p0 = project(center + (std::cos(a0) * ax[0] + std::sin(a0) * ax[1]) * radius);
            const auto p1 = project(center + (std::cos(a1) * ax[0] + std::sin(a1) * ax[1]) * radius);
            if (p0 && p1)
            {
                drawList->AddLine(*p0, *p1, color, 1.0f);
            }
        }
    }
}

// Draw a wireframe cone for spot lights.
void DrawLightConeGizmo(
    ImDrawList* drawList,
    const glm::vec3& apex,
    const glm::vec3& direction,
    float range,
    float outerAngleDegrees,
    const glm::mat4& viewProjection,
    const ViewportOverlayRect& viewportRect,
    ImU32 color)
{
    const float halfAngle = glm::radians(outerAngleDegrees);
    const float capRadius = range * std::tan(halfAngle);

    // Build a local frame for the cone cap
    glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(direction, up)) > 0.99f)
        up = glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 right = glm::normalize(glm::cross(direction, up));
    const glm::vec3 upDir = glm::normalize(glm::cross(right, direction));
    const glm::vec3 capCenter = apex + direction * range;

    constexpr int kSegments = 16;
    const auto project = [&](glm::vec3 p) -> std::optional<ImVec2>
    {
        ImVec2 s;
        if (!ProjectWorldPointToViewport(p, viewProjection, viewportRect, s))
            return std::nullopt;
        return s;
    };

    // Draw cap circle
    for (int i = 0; i < kSegments; ++i)
    {
        const float a0 = (static_cast<float>(i) / kSegments) * 2.0f * 3.14159265f;
        const float a1 = (static_cast<float>(i + 1) / kSegments) * 2.0f * 3.14159265f;
        const auto p0 = project(capCenter + (std::cos(a0) * right + std::sin(a0) * upDir) * capRadius);
        const auto p1 = project(capCenter + (std::cos(a1) * right + std::sin(a1) * upDir) * capRadius);
        if (p0 && p1)
            drawList->AddLine(*p0, *p1, color, 1.0f);
    }

    // Draw 4 edge lines from apex to cap rim
    for (int i = 0; i < 4; ++i)
    {
        const float a = static_cast<float>(i) * (3.14159265f * 0.5f);
        const auto pApex = project(apex);
        const auto pCap = project(capCenter + (std::cos(a) * right + std::sin(a) * upDir) * capRadius);
        if (pApex && pCap)
            drawList->AddLine(*pApex, *pCap, color, 1.0f);
    }
}

// Draw a wireframe rectangle for area lights.
void DrawLightAreaGizmo(
    ImDrawList* drawList,
    const glm::mat4& transform,
    float width,
    float height,
    const glm::mat4& viewProjection,
    const ViewportOverlayRect& viewportRect,
    ImU32 color)
{
    const float hw = width * 0.5f;
    const float hh = height * 0.5f;

    const glm::vec4 corners[4] = {
        {-hw, -hh, 0.0f, 1.0f},
        {hw, -hh, 0.0f, 1.0f},
        {hw, hh, 0.0f, 1.0f},
        {-hw, hh, 0.0f, 1.0f}};

    std::array<std::optional<ImVec2>, 4> projected;
    for (int i = 0; i < 4; ++i)
    {
        ImVec2 s;
        const glm::vec3 worldPt = glm::vec3(transform * corners[i]);
        if (ProjectWorldPointToViewport(worldPt, viewProjection, viewportRect, s))
            projected[i] = s;
    }

    for (int i = 0; i < 4; ++i)
    {
        int j = (i + 1) % 4;
        if (projected[i] && projected[j])
            drawList->AddLine(*projected[i], *projected[j], color, 1.5f);
    }

    // Draw normal arrow
    const glm::vec3 center = glm::vec3(transform * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    const glm::vec3 arrowTip = glm::vec3(transform * glm::vec4(0.0f, 0.0f, -0.5f, 1.0f));
    ImVec2 sc, sa;
    if (ProjectWorldPointToViewport(center, viewProjection, viewportRect, sc) &&
        ProjectWorldPointToViewport(arrowTip, viewProjection, viewportRect, sa))
    {
        drawList->AddLine(sc, sa, color, 1.5f);
    }
}

// Draw a directional light arrow.
void DrawLightDirectionalGizmo(
    ImDrawList* drawList,
    const glm::vec3& origin,
    const glm::vec3& direction,
    float length,
    const glm::mat4& viewProjection,
    const ViewportOverlayRect& viewportRect,
    ImU32 color)
{
    const glm::vec3 tip = origin + direction * length;
    ImVec2 so, st;
    if (!ProjectWorldPointToViewport(origin, viewProjection, viewportRect, so) ||
        !ProjectWorldPointToViewport(tip, viewProjection, viewportRect, st))
    {
        return;
    }
    drawList->AddLine(so, st, color, 1.5f);

    // Draw 3 parallel rays offset from origin
    glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(direction, up)) > 0.99f)
        up = glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 right = glm::normalize(glm::cross(direction, up));
    const float offset = 0.3f;
    for (int k = -1; k <= 1; k += 2)
    {
        const glm::vec3 off = right * (static_cast<float>(k) * offset);
        ImVec2 s0, s1;
        if (ProjectWorldPointToViewport(origin + off, viewProjection, viewportRect, s0) &&
            ProjectWorldPointToViewport(tip + off, viewProjection, viewportRect, s1))
        {
            drawList->AddLine(s0, s1, color, 1.0f);
        }
    }
}

// Draw all scene light gizmo wireframes in the viewport.
void DrawLightGizmos(
    const IEditorWorld& scene,
    const ViewportMatrices& matrices,
    const ViewportOverlayRect& viewportRect)
{
    if (viewportRect.size.x <= 0.0f || viewportRect.size.y <= 0.0f || viewportRect.drawList == nullptr)
    {
        return;
    }

    const glm::mat4 viewProjection = matrices.projection * matrices.view;
    const bool hasSelection = scene.HasSelection();
    ImDrawList* drawList = viewportRect.drawList;

    scene.ForEachLight([&](entt::entity entity, const TagComponent&, const TransformComponent& transform, const LightComponent& light)
                       {
                           const glm::mat4 modelMat = scene.GetModelMatrix(entity);
                           const glm::vec3 worldPos = glm::vec3(modelMat[3]);
                           const bool isSelected = hasSelection && scene.IsSelected(entity);
                           const ImU32 baseColor = GetLightTypeColor(light.type);
                           const ImU32 color = isSelected
                                                   ? IM_COL32(255, 196, 64, 255)
                                                   : ImGui::ColorConvertFloat4ToU32(ImVec4(
                                                         ((baseColor >> 0) & 0xFF) / 255.0f,
                                                         ((baseColor >> 8) & 0xFF) / 255.0f,
                                                         ((baseColor >> 16) & 0xFF) / 255.0f,
                                                         0.6f));

                           // Icon
                           ImVec2 iconPos;
                           if (ProjectWorldPointToViewport(worldPos, viewProjection, viewportRect, iconPos))
                           {
                               DrawLightViewportIcon(drawList, iconPos, light.type, isSelected, 1.0f);
                           }

                           // Type-specific wireframe (only when selected, or always for small gizmo)
                           if (light.type == LightType::Point)
                           {
                               DrawLightSphereGizmo(drawList, worldPos, light.range, viewProjection, viewportRect, color);
                           }
                           else if (light.type == LightType::Spot)
                           {
                               // Compute forward direction from transform rotation
                               glm::mat4 rotMat(1.0f);
                               rotMat = glm::rotate(rotMat, glm::radians(transform.rotationDegrees.x), glm::vec3(1, 0, 0));
                               rotMat = glm::rotate(rotMat, glm::radians(transform.rotationDegrees.y), glm::vec3(0, 1, 0));
                               rotMat = glm::rotate(rotMat, glm::radians(transform.rotationDegrees.z), glm::vec3(0, 0, 1));
                               const glm::vec3 dir = glm::normalize(glm::vec3(rotMat * glm::vec4(0, -1, 0, 0)));
                               DrawLightConeGizmo(drawList, worldPos, dir, light.range, light.spotOuterAngleDegrees,
                                                  viewProjection, viewportRect, color);
                           }
                           else if (light.type == LightType::Area)
                           {
                               DrawLightAreaGizmo(drawList, modelMat, light.areaSize.x, light.areaSize.y,
                                                  viewProjection, viewportRect, color);
                           }
                           else if (light.type == LightType::Directional)
                           {
                               glm::mat4 rotMat(1.0f);
                               rotMat = glm::rotate(rotMat, glm::radians(transform.rotationDegrees.x), glm::vec3(1, 0, 0));
                               rotMat = glm::rotate(rotMat, glm::radians(transform.rotationDegrees.y), glm::vec3(0, 1, 0));
                               rotMat = glm::rotate(rotMat, glm::radians(transform.rotationDegrees.z), glm::vec3(0, 0, 1));
                               const glm::vec3 dir = glm::normalize(glm::vec3(rotMat * glm::vec4(0, -1, 0, 0)));
                               DrawLightDirectionalGizmo(drawList, worldPos, dir, 2.0f, viewProjection, viewportRect, color);
                           }
                       });
}

// Extend projected centers with light entities for picking.
void AppendLightProjectedCenters(
    const IEditorWorld& scene,
    const ViewportMatrices& matrices,
    const ViewportOverlayRect& viewportRect,
    std::vector<ProjectedEntityCenter>& projectedCenters)
{
    if (viewportRect.size.x <= 0.0f || viewportRect.size.y <= 0.0f)
        return;

    const glm::mat4 viewProjection = matrices.projection * matrices.view;

    scene.ForEachLight([&](entt::entity entity, const TagComponent&, const TransformComponent&, const LightComponent&)
                       {
                           const glm::vec3 worldPos = glm::vec3(scene.GetModelMatrix(entity) * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                           ProjectedEntityCenter projected{};
                           if (ProjectLightCenter(entity, worldPos, viewProjection, viewportRect, projected))
                           {
                               projectedCenters.push_back(projected);
                           }
                       });
}

glm::vec3 UnprojectToGroundPlane(
    const ImVec2& mousePos,
    const ViewportOverlayRect& viewportRect,
    const ViewportMatrices& matrices,
    const Camera& camera)
{
    if (viewportRect.size.x <= 0.0f || viewportRect.size.y <= 0.0f)
    {
        return camera.position;
    }

    const float ndcX = (mousePos.x - viewportRect.origin.x) / viewportRect.size.x * 2.0f - 1.0f;
    const float ndcY = 1.0f - (mousePos.y - viewportRect.origin.y) / viewportRect.size.y * 2.0f;

    const glm::mat4 invViewProj = glm::inverse(matrices.projection * matrices.view);
    const glm::vec4 nearClip = invViewProj * glm::vec4(ndcX, ndcY, -1.0f, 1.0f);
    const glm::vec4 farClip = invViewProj * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    const glm::vec3 nearWorld = glm::vec3(nearClip) / nearClip.w;
    const glm::vec3 farWorld = glm::vec3(farClip) / farClip.w;
    const glm::vec3 rayDir = glm::normalize(farWorld - nearWorld);

    if (std::abs(rayDir.y) > 0.0001f)
    {
        const float t = -nearWorld.y / rayDir.y;
        if (t > 0.0f && t < 10000.0f)
        {
            return nearWorld + rayDir * t;
        }
    }

    return camera.position + rayDir * 10.0f;
}
}

void EditorUiController::DrawViewportPanel(
    Camera& camera,
    ViewportMatrices& matrices,
    IEditorWorld& scene,
    ImTextureID viewportTextureId,
    RenderBackendType currentBackendType,
    EditorUiFrameResult& result)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("Viewport", &m_showViewportWindow))
    {
        const bool flipViewportImageY = false;
        const ViewportOverlayRect viewportRect = BuildViewportOverlayRect(viewportTextureId, flipViewportImageY);
        DrawViewportOverlay(viewportRect, viewportTextureId);

        if (const ImGuiPayload* dragPayload = ImGui::GetDragDropPayload();
            dragPayload != nullptr && dragPayload->IsDataType("ASSET_MODEL_PATH") && viewportRect.hovered)
        {
            const std::string hoveredPath = ReadDragDropPayloadString(*dragPayload);
            if (!hoveredPath.empty())
            {
                result.actions.hoveredViewportModel = EditorUiActions::ViewportModelPlacement{
                    hoveredPath,
                    UnprojectToGroundPlane(ImGui::GetIO().MousePos, viewportRect, matrices, camera)};
            }
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MODEL_PATH"))
            {
                const std::string droppedPath = ReadDragDropPayloadString(*payload);
                if (!droppedPath.empty())
                {
                    result.actions.droppedViewportModel = EditorUiActions::ViewportModelPlacement{
                        droppedPath,
                        UnprojectToGroundPlane(ImGui::GetIO().MousePos, viewportRect, matrices, camera)};
                }
            }
            ImGui::EndDragDropTarget();
        }

        result.viewportExtent = BuildViewportExtent(viewportRect);
        result.viewportInteractionRect = SDL_FRect{
            viewportRect.origin.x,
            viewportRect.origin.y,
            viewportRect.size.x,
            viewportRect.size.y};
        result.viewportAllowsMouseInteraction = viewportRect.size.x > 0.0f && viewportRect.size.y > 0.0f;
        HandleViewportShortcuts(scene, camera, viewportRect);
        RefreshViewportMatrices(camera, matrices, scene, result.viewportExtent, currentBackendType);
        DrawViewManipulator(camera, matrices, viewportRect);
        RefreshViewportMatrices(camera, matrices, scene, result.viewportExtent, currentBackendType);
        DrawGizmoOverlay(scene, matrices, viewportRect, m_gizmoDragSnapState);
        DrawLightGizmos(scene, matrices, viewportRect);
        std::vector<ProjectedEntityCenter> projectedCenters = ProjectSceneCenters(scene, matrices, viewportRect);
        AppendLightProjectedCenters(scene, matrices, viewportRect, projectedCenters);
        HandleViewportSelection(scene, projectedCenters, viewportRect);
        DrawViewportSelectionOverlay(scene, matrices, viewportRect, projectedCenters);
        ImGui::SetCursorScreenPos(ImVec2(viewportRect.origin.x + 12.0f, viewportRect.origin.y + 12.0f));
        ImGui::BeginGroup();
        ImGui::TextUnformatted("Viewport");
        ImGui::TextUnformatted("F to frame, R toggles combined/scale gizmo, drag assets here to place");
        ImGui::Text("Render Size: %u x %u", result.viewportExtent.width, result.viewportExtent.height);
        ImGui::Text("Viewport FPS: %.1f", ImGui::GetIO().Framerate);
        ImGui::EndGroup();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}
}
