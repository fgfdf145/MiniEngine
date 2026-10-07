#include "viewport_panel.h"

#include <engine/editor/editor_ui.h>
#include <engine/editor/ui/editor_gt7_hud.h>
#include <engine/editor/ui/editor_ui_internal.h>
#include <engine/editor/ui/editor_vehicle_overlay.h>
#include <engine/editor/ui/framework/editor_window_manager.h>
#include <engine/editor/ui/panels/suspension_rigs_panel.h>

#include <engine/core/log/log.h>
#include <engine/logic/editor_world.h>
#include <engine/logic/world_bounds.h>
#include <IconsPhosphor.h>
#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace me
{

namespace
{
// Overlay sizes below are at UI scale 1 and are multiplied by the effective UI scale (DPI times
// the user multiplier), like the ImGui style, so they keep their size relative to the text.
constexpr float kSelectionCenterHitRadiusPixels = 20.0f;
constexpr float kSelectionBoundsHitPaddingPixels = 6.0f;
constexpr float kSelectionOutlineThickness = 2.0f;
constexpr float kLightIconRadiusPixels = 10.0f;
constexpr float kLightIconHitHalfSizePixels = 16.0f;
constexpr float kLightSelectionRingRadiusPixels = 14.0f;
constexpr float kViewCubeSizePixels = 128.0f;
constexpr float kViewCubeMarginPixels = 16.0f;
constexpr float kOverlayTextMarginPixels = 12.0f;
constexpr float kMinimapSizePixels = 220.0f;
constexpr float kMinimapMarginPixels = 16.0f;
// How far the minimap reaches from its centre to its edges' midpoints.
constexpr float kMinimapRadiusMetres = 250.0f;

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

// fixedAspect: the width over the height of a fixed output resolution, which the image keeps, as
// large as fits and centred, with black bars round it. Unset, the image fills the panel.
ViewportOverlayRect BuildViewportOverlayRect(ImTextureID viewportTextureId, bool flipViewportImageY, std::optional<float> fixedAspect)
{
    ViewportOverlayRect rect{};
    rect.drawList = ImGui::GetWindowDrawList();

    ImVec2 available = ImGui::GetContentRegionAvail();
    available.x = std::max(available.x, 1.0f);
    available.y = std::max(available.y, 1.0f);

    ImVec2 imageSize = available;
    if (fixedAspect.has_value() && *fixedAspect > 0.0f)
    {
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        rect.drawList->AddRectFilled(cursor, ImVec2(cursor.x + available.x, cursor.y + available.y), IM_COL32(0, 0, 0, 255));
        imageSize = available.x / available.y > *fixedAspect ? ImVec2(available.y * *fixedAspect, available.y)
                                                               : ImVec2(available.x, available.x / *fixedAspect);
        // Whole points, so the image's edges fall on pixels.
        imageSize.x = std::max(std::floor(imageSize.x), 1.0f);
        imageSize.y = std::max(std::floor(imageSize.y), 1.0f);
        ImGui::SetCursorScreenPos(ImVec2(
            cursor.x + std::floor((available.x - imageSize.x) * 0.5f),
            cursor.y + std::floor((available.y - imageSize.y) * 0.5f)));
    }

    if (viewportTextureId)
    {
        const ImVec2 uv0 = flipViewportImageY ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
        const ImVec2 uv1 = flipViewportImageY ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
        ImGui::Image(viewportTextureId, imageSize, uv0, uv1);
    }
    else
    {
        ImGui::Dummy(imageSize);
    }

    rect.origin = ImGui::GetItemRectMin();
    rect.size = ImGui::GetItemRectSize();
    rect.hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    rect.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    return rect;
}

// The panel's size is in points; the scene renders at the display's pixels (DisplayFramebufferScale,
// 2 on Retina) times the render scale.
RenderExtent BuildViewportExtent(const ViewportOverlayRect& rect, float renderScale)
{
    return ScaleViewportExtent(rect.size.x, rect.size.y, ImGui::GetIO().DisplayFramebufferScale.x, renderScale);
}

// The size the scene renders at whatever the panel's size: the backend's (--viewport-size, a
// recording) first, then the viewport resolution setting, scaled by the render scale. Unset, the
// panel's size decides.
std::optional<RenderExtent> ResolveFixedViewportExtent(const EditorSharedState& state, float renderScale)
{
    if (state.forcedViewportExtent.has_value() && state.forcedViewportExtent->IsValid())
    {
        return state.forcedViewportExtent;
    }
    return state.renderDebug.viewportResolution.Extent(renderScale);
}

// The aspect a fixed size is shown at: the setting's own, so the render scale's rounding does not
// stretch it.
std::optional<float> FixedViewportAspect(const EditorSharedState& state, const std::optional<RenderExtent>& fixedExtent)
{
    if (!fixedExtent.has_value())
    {
        return std::nullopt;
    }
    if (!state.forcedViewportExtent.has_value())
    {
        const ViewportResolutionSettings& resolution = state.renderDebug.viewportResolution;
        return static_cast<float>(std::clamp(resolution.width, ViewportResolutionSettings::kMinSize, ViewportResolutionSettings::kMaxSize)) /
               static_cast<float>(std::clamp(resolution.height, ViewportResolutionSettings::kMinSize, ViewportResolutionSettings::kMaxSize));
    }
    return static_cast<float>(fixedExtent->width) / static_cast<float>(fixedExtent->height);
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

    rect.drawList->AddRect(rect.origin, max, IM_COL32(255, 255, 255, 48), 0.0f, 1.0f);
}

// The driven car as the driving HUD shows it. The pedals are what the car takes: pulling the throttle
// back brakes, except in the automatic's reverse, where it drives and pushing it brakes.
Gt7HudInput BuildGt7HudInput(const VehicleDriveStatus& vehicle, double time)
{
    const VehicleTelemetry& telemetry = vehicle.telemetry;
    const VehicleControls& controls = vehicle.controls;
    Gt7HudInput input;
    input.speedKmh = std::abs(telemetry.forwardSpeed) * 3.6f;
    input.rpm = telemetry.engineRpm;
    input.maxRpm = vehicle.engineMaxRpm;
    input.gear = telemetry.gear;
    input.manualGearbox = vehicle.manualGearbox;
    const bool reversing = telemetry.gear < 0 && !vehicle.manualGearbox;
    const float drive = reversing ? -controls.throttle : controls.throttle;
    input.throttle = std::clamp(drive, 0.0f, 1.0f);
    input.brake = std::clamp(std::max(controls.brake, -drive), 0.0f, 1.0f);
    input.steering = controls.steering;
    input.handBrake = controls.handBrake > 0.05f;
    input.absFitted = vehicle.absFitted;
    input.absActive = telemetry.absActive;
    input.tcsFitted = vehicle.tractionControlFitted;
    input.tcsActive = telemetry.tractionControlCut;
    input.counterSteerAssist = vehicle.counterSteerAssist;
    input.turbo = vehicle.turbo;
    input.boostBar = telemetry.turboBoost;
    input.odometerKm = vehicle.odometerMetres / 1000.0;
    input.frontTyre = vehicle.frontTyre;
    input.rearTyre = vehicle.rearTyre;
    input.time = time;
    return input;
}

void DrawFullscreenViewportHud(
    const ViewportOverlayRect& rect, float uiScale, double secondsSinceEntered, const VehicleDriveStatus& vehicle, bool drivingHud)
{
    ImDrawList* drawList = rect.drawList;
    const float margin = kOverlayTextMarginPixels * uiScale;
    const ImU32 textColor = IM_COL32(255, 255, 255, 230);
    const ImU32 shadowColor = IM_COL32(0, 0, 0, 160);
    const auto drawText = [&](const ImVec2& position, const std::string& text)
    {
        drawList->AddText(ImVec2(position.x + 1.0f, position.y + 1.0f), shadowColor, text.c_str());
        drawList->AddText(position, textColor, text.c_str());
    };

    // The way out, for the first seconds only.
    constexpr double kHintSeconds = 3.0;
    if (secondsSinceEntered < kHintSeconds)
    {
        drawText(ImVec2(rect.origin.x + margin, rect.origin.y + margin), "F11 or Esc: leave fullscreen");
    }

    // While driving without the driving HUD, the speed and gear at the bottom right.
    if (vehicle.active && !drivingHud)
    {
        const VehicleTelemetry& telemetry = vehicle.telemetry;
        const std::string gear = telemetry.gear < 0 ? "R" : telemetry.gear == 0 ? "N" : std::to_string(telemetry.gear);
        const std::string text = std::to_string(static_cast<int>(std::abs(telemetry.forwardSpeed) * 3.6f + 0.5f)) + " km/h   " + gear;
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        drawText(ImVec2(rect.origin.x + rect.size.x - size.x - margin, rect.origin.y + rect.size.y - size.y - margin), text);
    }
}

// The scene's map (SceneMinimap) at the viewport's bottom left (top right when `topRight`, clear of the
// driving HUD), centred on the player and turned so the way they face is up, as a game's radar: an
// arrow at the centre for the player, N on the border towards north.
void DrawMinimap(
    const ViewportOverlayRect& rect,
    float uiScale,
    ImTextureID texture,
    const SceneMinimap& minimap,
    const glm::vec3& position,
    const glm::vec3& heading,
    bool topRight = false)
{
    ImDrawList* drawList = rect.drawList;
    if (drawList == nullptr || !texture || !minimap.IsValid())
    {
        return;
    }
    // Never more than 40% of the viewport's smaller side, and not at all in a sliver of a viewport.
    const float size = std::min(kMinimapSizePixels * uiScale, std::min(rect.size.x, rect.size.y) * 0.4f);
    if (size < 48.0f)
    {
        return;
    }
    const float margin = kMinimapMarginPixels * uiScale;
    const float half = size * 0.5f;
    const ImVec2 center = topRight ? ImVec2(rect.origin.x + rect.size.x - margin - half, rect.origin.y + margin + half)
                                   : ImVec2(rect.origin.x + margin + half, rect.origin.y + rect.size.y - margin - half);

    // Screen up is the heading across the ground, screen right is its right. The picture's u runs with
    // world X and its v with world Z, so "right" is the heading turned a quarter clockwise in (x, z).
    glm::vec2 forward(heading.x, heading.z);
    forward = glm::length(forward) > 1e-4f ? glm::normalize(forward) : glm::vec2(0.0f, -1.0f);
    const glm::vec2 right(-forward.y, forward.x);
    const float metresPerPixel = kMinimapRadiusMetres / half;
    const glm::vec2 focus(position.x, position.z);
    const glm::vec2 worldSize = minimap.worldMax - minimap.worldMin;
    // The picture's uv under a point this far right of and below the centre, in pixels.
    const auto uvAt = [&](float x, float y)
    {
        const glm::vec2 world = focus + (right * x - forward * y) * metresPerPixel;
        const glm::vec2 uv = (world - minimap.worldMin) / worldSize;
        return ImVec2(uv.x, uv.y);
    };

    const ImVec2 min(center.x - half, center.y - half);
    const ImVec2 max(center.x + half, center.y + half);
    const float border = 2.0f * uiScale;
    drawList->AddRectFilled(ImVec2(min.x - border, min.y - border), ImVec2(max.x + border, max.y + border), IM_COL32(0, 0, 0, 170));
    drawList->AddImageQuad(
        texture,
        min,
        ImVec2(max.x, min.y),
        max,
        ImVec2(min.x, max.y),
        uvAt(-half, -half),
        uvAt(half, -half),
        uvAt(half, half),
        uvAt(-half, half));
    drawList->AddRect(min, max, IM_COL32(255, 255, 255, 90), 0.0f, 1.0f);

    // The player: an arrow pointing up (the way they face), white with a dark outline.
    const float arrow = 7.0f * uiScale;
    const ImVec2 tip(center.x, center.y - arrow * 1.3f);
    const ImVec2 left(center.x - arrow, center.y + arrow);
    const ImVec2 notch(center.x, center.y + arrow * 0.45f);
    const ImVec2 rightCorner(center.x + arrow, center.y + arrow);
    drawList->AddTriangleFilled(tip, left, notch, IM_COL32(255, 255, 255, 255));
    drawList->AddTriangleFilled(tip, notch, rightCorner, IM_COL32(255, 255, 255, 255));
    const ImVec2 outline[] = {tip, rightCorner, notch, left};
    drawList->AddPolyline(outline, 4, IM_COL32(0, 0, 0, 220), 1.5f * uiScale, ImDrawFlags_Closed);

    // North (-Z) on the screen, pushed out to just inside the border.
    const glm::vec2 north(0.0f, -1.0f);
    glm::vec2 towardsNorth(glm::dot(north, right), -glm::dot(north, forward));
    const float badge = 9.0f * uiScale;
    towardsNorth *= (half - badge - border) / std::max(std::abs(towardsNorth.x), std::abs(towardsNorth.y));
    const ImVec2 northCenter(center.x + towardsNorth.x, center.y + towardsNorth.y);
    drawList->AddCircleFilled(northCenter, badge, IM_COL32(20, 20, 20, 220));
    const ImVec2 textSize = ImGui::CalcTextSize("N");
    drawList->AddText(ImVec2(northCenter.x - textSize.x * 0.5f, northCenter.y - textSize.y * 0.5f), IM_COL32(255, 255, 255, 255), "N");
}

// While recording, a blinking red dot, the video's length and size at the top centre; for a few
// seconds after, where it was saved or why it stopped. Drawn by ImGui over the viewport, so it is
// not in the video.
void DrawVideoRecordingIndicator(const ViewportOverlayRect& rect, float uiScale, const VideoRecordingIndicator& recording)
{
    constexpr double kMessageSeconds = 6.0;
    std::string text;
    ImU32 textColor = IM_COL32(255, 255, 255, 235);
    if (recording.active)
    {
        const int totalSeconds = static_cast<int>(recording.seconds);
        text = fmt::format(
            "REC  {:02}:{:02}  {:.1f} MB", totalSeconds / 60, totalSeconds % 60,
            static_cast<double>(recording.bytes) / (1024.0 * 1024.0));
        if (recording.droppedFrames > 0)
        {
            text += fmt::format("  ({} frames dropped)", recording.droppedFrames);
        }
    }
    else if (!recording.message.empty() &&
             std::chrono::duration<double>(std::chrono::steady_clock::now() - recording.messageTime).count() < kMessageSeconds)
    {
        text = recording.message;
        textColor = recording.messageIsError ? IM_COL32(255, 120, 110, 255) : IM_COL32(150, 230, 150, 255);
    }
    if (text.empty() || rect.drawList == nullptr)
    {
        return;
    }

    ImDrawList* drawList = rect.drawList;
    const float margin = kOverlayTextMarginPixels * uiScale;
    const float padding = 6.0f * uiScale;
    const float dotRadius = recording.active ? 5.0f * uiScale : 0.0f;
    const float dotSpace = recording.active ? dotRadius * 2.0f + padding : 0.0f;
    const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    const float width = padding * 2.0f + dotSpace + textSize.x;
    const ImVec2 min(rect.origin.x + (rect.size.x - width) * 0.5f, rect.origin.y + margin);
    const ImVec2 max(min.x + width, min.y + textSize.y + padding * 2.0f);
    drawList->AddRectFilled(min, max, IM_COL32(0, 0, 0, 170), 4.0f * uiScale);
    if (recording.active)
    {
        // On for most of each second, so it reads as recording rather than as a warning.
        const bool dotOn = std::fmod(ImGui::GetTime(), 1.0) < 0.7;
        const ImVec2 center(min.x + padding + dotRadius, (min.y + max.y) * 0.5f);
        drawList->AddCircleFilled(center, dotRadius, dotOn ? IM_COL32(235, 40, 40, 255) : IM_COL32(110, 30, 30, 255));
    }
    drawList->AddText(ImVec2(min.x + padding + dotSpace, min.y + padding), textColor, text.c_str());
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

entt::entity PickHoveredEntity(const std::vector<ProjectedEntityCenter>& projectedCenters, float uiScale)
{
    const ImVec2 mousePosition = ImGui::GetMousePos();
    entt::entity hoveredEntity = entt::null;
    float bestDepth = std::numeric_limits<float>::max();
    const float hitRadius = kSelectionCenterHitRadiusPixels * uiScale;
    const float hitRadiusSquared = hitRadius * hitRadius;
    const float boundsPadding = kSelectionBoundsHitPaddingPixels * uiScale;

    for (const ProjectedEntityCenter& projectedCenter : projectedCenters)
    {
        const bool insideBounds =
            mousePosition.x >= projectedCenter.min.x - boundsPadding &&
            mousePosition.x <= projectedCenter.max.x + boundsPadding &&
            mousePosition.y >= projectedCenter.min.y - boundsPadding &&
            mousePosition.y <= projectedCenter.max.y + boundsPadding;
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
    const float radius = kLightIconRadiusPixels * scale;
    const ImU32 fillColor = selected
                                ? IM_COL32(255, 196, 64, 220)
                                : IM_COL32(255, 255, 255, 80);

    drawList->AddCircleFilled(screenPos, radius, fillColor);
    drawList->AddCircle(screenPos, radius, typeColor, 16, (selected ? 2.5f : 1.5f) * scale);

    const char* badge = "L";
    if (type == LightType::Directional)
        badge = "D";
    else if (type == LightType::Spot)
        badge = "S";
    else if (type == LightType::Area)
        badge = "A";
    else if (type == LightType::Ambient)
        badge = "*";
    else if (type == LightType::Hemisphere)
        badge = "H";

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
    const ViewportOverlayRect& viewportRect,
    float uiScale)
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
    viewportRect.drawList->AddCircle(
        screenPos,
        kLightSelectionRingRadiusPixels * uiScale,
        kSelectionOutlineColor,
        24,
        kSelectionOutlineThickness * uiScale);
    DrawLightViewportIcon(viewportRect.drawList, screenPos, light.type, true, uiScale);
}

// The renderer's selection outline over the viewport image, as Blender outlines the active object.
// Drawn every frame, selection or none: the image is transparent without one, and the selection
// this frame's clicks make is the one the renderer outlines after them.
void DrawViewportSelectionOutline(const ViewportOverlayRect& viewportRect, ImTextureID outlineTexture)
{
    if (viewportRect.drawList == nullptr || !outlineTexture)
    {
        return;
    }
    viewportRect.drawList->AddImage(
        outlineTexture,
        viewportRect.origin,
        ImVec2(viewportRect.origin.x + viewportRect.size.x, viewportRect.origin.y + viewportRect.size.y));
}

// outlined: the renderer outlines the selection's meshes (DrawViewportSelectionOutline), so a model
// needs no bounding box.
void DrawViewportSelectionOverlay(
    const IEditorWorld& scene,
    const ViewportMatrices& matrices,
    const ViewportOverlayRect& viewportRect,
    float uiScale,
    bool outlined)
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
        DrawLightSelectionIndicator(scene, selected, matrices, viewportRect, uiScale);
        return;
    }
    if (outlined)
    {
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
            kSelectionOutlineThickness * uiScale);
    }

    for (const ImVec2& corner : projectedSelectionCorners)
    {
        drawList->AddCircleFilled(corner, 2.5f * uiScale, kSelectionOutlineColor);
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
    matrices.renderProjection = camera.GetProjectionMatrix(
        viewportExtent, invertRenderYAxis, useZeroToOneDepth, UsesReverseRenderDepth(currentBackendType));
    matrices.model =
        scene.HasSelection() ? scene.GetModelMatrix(scene.GetSelectedEntity()) : glm::mat4(1.0f);
}

void HandleViewportShortcuts(IEditorWorld& scene, const ViewportOverlayRect& viewportRect)
{
    ImGuiIO& io = ImGui::GetIO();
    // Ctrl+R and the like are editor commands (ProcessCommandShortcuts), not these keys.
    if (io.WantCaptureKeyboard || io.KeyCtrl || io.KeyAlt || io.KeySuper || !scene.HasSelection() || !viewportRect.focused)
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
}

void HandleViewportSelection(
    IEditorWorld& scene,
    const std::vector<ProjectedEntityCenter>& projectedCenters,
    const ViewportOverlayRect& viewportRect,
    float uiScale)
{
    if (!viewportRect.hovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        return;
    }

    if (scene.HasSelection() && ImGuizmo::IsUsing())
    {
        return;
    }

    scene.SetSelectedEntity(PickHoveredEntity(projectedCenters, uiScale));
}

void DrawViewManipulator(
    Camera& camera,
    ViewportMatrices& matrices,
    const ViewportOverlayRect& viewportRect,
    float uiScale)
{
    if (viewportRect.size.x <= 0.0f || viewportRect.size.y <= 0.0f || viewportRect.drawList == nullptr)
    {
        return;
    }

    ImGuizmo::SetDrawlist(viewportRect.drawList);
    const glm::mat4 viewBefore = matrices.view;
    const float cubeSize = kViewCubeSizePixels * uiScale;
    const float cubeMargin = kViewCubeMarginPixels * uiScale;
    ImGuizmo::ViewManipulate(
        glm::value_ptr(matrices.view),
        7.5f,
        ImVec2(viewportRect.origin.x + viewportRect.size.x - cubeSize - cubeMargin, viewportRect.origin.y + cubeMargin),
        ImVec2(cubeSize, cubeSize),
        IM_COL32(32, 32, 32, 180));
    if (matrices.view != viewBefore)
    {
        camera.SetFromViewMatrix(matrices.view);
        matrices.view = camera.GetViewMatrix();
    }
}

// ImGuizmo sizes its handle lines, arrows and circles in pixels; scale them from its defaults.
void ApplyImGuizmoStyleScale(float uiScale)
{
    static const ImGuizmo::Style kDefaultStyle{};
    ImGuizmo::Style& style = ImGuizmo::GetStyle();
    style.TranslationLineThickness = kDefaultStyle.TranslationLineThickness * uiScale;
    style.TranslationLineArrowSize = kDefaultStyle.TranslationLineArrowSize * uiScale;
    style.RotationLineThickness = kDefaultStyle.RotationLineThickness * uiScale;
    style.RotationOuterLineThickness = kDefaultStyle.RotationOuterLineThickness * uiScale;
    style.ScaleLineThickness = kDefaultStyle.ScaleLineThickness * uiScale;
    style.ScaleLineCircleSize = kDefaultStyle.ScaleLineCircleSize * uiScale;
    style.HatchedAxisLineThickness = kDefaultStyle.HatchedAxisLineThickness * uiScale;
    style.CenterCircleSize = kDefaultStyle.CenterCircleSize * uiScale;
}

void DrawGizmoOverlay(
    IEditorWorld& scene,
    ViewportMatrices& matrices,
    const ViewportOverlayRect& viewportRect,
    GizmoDragSnapState& dragSnapState,
    float uiScale)
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

    ApplyImGuizmoStyleScale(uiScale);
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
    float uiScale,
    ProjectedEntityCenter& out)
{
    ImVec2 screenPos;
    if (!ProjectWorldPointToViewport(worldPos, viewProjection, viewportRect, screenPos))
    {
        return false;
    }

    const float iconHalfSize = kLightIconHitHalfSizePixels * uiScale;
    out.entity = entity;
    out.center = screenPos;
    out.min = ImVec2(screenPos.x - iconHalfSize, screenPos.y - iconHalfSize);
    out.max = ImVec2(screenPos.x + iconHalfSize, screenPos.y + iconHalfSize);

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
    ImU32 color,
    float thickness)
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
                drawList->AddLine(*p0, *p1, color, thickness);
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
    ImU32 color,
    float thickness)
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
            drawList->AddLine(*p0, *p1, color, thickness);
    }

    // Draw 4 edge lines from apex to cap rim
    for (int i = 0; i < 4; ++i)
    {
        const float a = static_cast<float>(i) * (3.14159265f * 0.5f);
        const auto pApex = project(apex);
        const auto pCap = project(capCenter + (std::cos(a) * right + std::sin(a) * upDir) * capRadius);
        if (pApex && pCap)
            drawList->AddLine(*pApex, *pCap, color, thickness);
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
    ImU32 color,
    float thickness)
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
            drawList->AddLine(*projected[i], *projected[j], color, 1.5f * thickness);
    }

    // Draw normal arrow
    const glm::vec3 center = glm::vec3(transform * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    const glm::vec3 arrowTip = glm::vec3(transform * glm::vec4(0.0f, 0.0f, -0.5f, 1.0f));
    ImVec2 sc, sa;
    if (ProjectWorldPointToViewport(center, viewProjection, viewportRect, sc) &&
        ProjectWorldPointToViewport(arrowTip, viewProjection, viewportRect, sa))
    {
        drawList->AddLine(sc, sa, color, 1.5f * thickness);
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
    ImU32 color,
    float thickness)
{
    const glm::vec3 tip = origin + direction * length;
    ImVec2 so, st;
    if (!ProjectWorldPointToViewport(origin, viewProjection, viewportRect, so) ||
        !ProjectWorldPointToViewport(tip, viewProjection, viewportRect, st))
    {
        return;
    }
    drawList->AddLine(so, st, color, 1.5f * thickness);

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
            drawList->AddLine(s0, s1, color, thickness);
        }
    }
}

// Draw all scene light gizmo wireframes in the viewport.
void DrawLightGizmos(
    const IEditorWorld& scene,
    const ViewportMatrices& matrices,
    const ViewportOverlayRect& viewportRect,
    float uiScale)
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
                               DrawLightViewportIcon(drawList, iconPos, light.type, isSelected, uiScale);
                           }

                           // Type-specific wireframe (only when selected, or always for small gizmo)
                           if (light.type == LightType::Point)
                           {
                               DrawLightSphereGizmo(drawList, worldPos, light.range, viewProjection, viewportRect, color, uiScale);
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
                                                  viewProjection, viewportRect, color, uiScale);
                           }
                           else if (light.type == LightType::Area)
                           {
                               DrawLightAreaGizmo(drawList, modelMat, light.areaSize.x, light.areaSize.y,
                                                  viewProjection, viewportRect, color, uiScale);
                           }
                           else if (light.type == LightType::Directional)
                           {
                               glm::mat4 rotMat(1.0f);
                               rotMat = glm::rotate(rotMat, glm::radians(transform.rotationDegrees.x), glm::vec3(1, 0, 0));
                               rotMat = glm::rotate(rotMat, glm::radians(transform.rotationDegrees.y), glm::vec3(0, 1, 0));
                               rotMat = glm::rotate(rotMat, glm::radians(transform.rotationDegrees.z), glm::vec3(0, 0, 1));
                               const glm::vec3 dir = glm::normalize(glm::vec3(rotMat * glm::vec4(0, -1, 0, 0)));
                               DrawLightDirectionalGizmo(drawList, worldPos, dir, 2.0f, viewProjection, viewportRect, color, uiScale);
                           }
                       });
}

// Extend projected centers with light entities for picking.
void AppendLightProjectedCenters(
    const IEditorWorld& scene,
    const ViewportMatrices& matrices,
    const ViewportOverlayRect& viewportRect,
    float uiScale,
    std::vector<ProjectedEntityCenter>& projectedCenters)
{
    if (viewportRect.size.x <= 0.0f || viewportRect.size.y <= 0.0f)
        return;

    const glm::mat4 viewProjection = matrices.projection * matrices.view;

    scene.ForEachLight([&](entt::entity entity, const TagComponent&, const TransformComponent&, const LightComponent&)
                       {
                           const glm::vec3 worldPos = glm::vec3(scene.GetModelMatrix(entity) * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                           ProjectedEntityCenter projected{};
                           if (ProjectLightCenter(entity, worldPos, viewProjection, viewportRect, uiScale, projected))
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

ViewportPanel::ViewportPanel()
    : EditorPanel("viewport", "Viewport", ICON_PH_MONITOR, EditorDockSlot::Center)
{
    Open();
}

bool ViewportPanel::ShouldDraw(const EditorContext& context) const
{
    return IsOpen() || context.state.commands.viewportFullscreen;
}

// Fullscreen: a window of its own over the whole screen with nothing of the editor around it, so the
// docked "Viewport" keeps its place in the layout.
const char* ViewportPanel::GetImGuiName(const EditorContext& context) const
{
    return context.state.commands.viewportFullscreen ? "Viewport##Fullscreen" : GetTitle().c_str();
}

bool ViewportPanel::IsClosable(const EditorContext& context) const
{
    return !context.state.commands.viewportFullscreen;
}

ImGuiWindowFlags ViewportPanel::GetWindowFlags(const EditorContext& context) const
{
    if (!context.state.commands.viewportFullscreen)
    {
        return ImGuiWindowFlags_None;
    }
    return ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
           ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollWithMouse;
}

void ViewportPanel::PreBegin(EditorContext& context)
{
    m_pushedStyleVars = 1;
    if (context.state.commands.viewportFullscreen)
    {
        const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(mainViewport->Pos);
        ImGui::SetNextWindowSize(mainViewport->Size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ++m_pushedStyleVars;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
}

void ViewportPanel::PostEnd(EditorContext& context)
{
    static_cast<void>(context);
    ImGui::PopStyleVar(m_pushedStyleVars);
    m_pushedStyleVars = 0;
}

void ViewportPanel::OnGui(EditorContext& context)
{
    EditorSharedState& state = context.state;
    Camera& camera = context.camera;
    ViewportMatrices& matrices = context.matrices;
    IEditorWorld& scene = context.scene;
    EditorUiFrameResult& result = context.result;
    const ImTextureID viewportTextureId = context.frame.viewportTextureId;
    const RenderBackendType currentBackendType = context.frame.backendType;
    const bool fullscreen = state.commands.viewportFullscreen;
    const bool flipViewportImageY = false;
    const float renderScale = state.DlssResolves() ? 1.0f : std::clamp(state.renderDebug.renderScale, 0.25f, 1.0f);
    const std::optional<RenderExtent> fixedExtent = ResolveFixedViewportExtent(state, renderScale);
    const ViewportOverlayRect viewportRect =
        BuildViewportOverlayRect(viewportTextureId, flipViewportImageY, FixedViewportAspect(state, fixedExtent));
    if (!fullscreen)
    {
        DrawViewportOverlay(viewportRect, viewportTextureId);
        // Under every other overlay, as in Blender.
        DrawViewportSelectionOutline(viewportRect, state.selectionOutlineTexture);
    }

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

    result.viewportExtent = fixedExtent.value_or(BuildViewportExtent(viewportRect, renderScale));
    const float displayWidthPixels = viewportRect.size.x * ImGui::GetIO().DisplayFramebufferScale.x;
    result.viewportOutputScale =
        displayWidthPixels >= 1.0f ? static_cast<float>(result.viewportExtent.width) / displayWidthPixels : 1.0f;
    result.viewportInteractionRect = SDL_FRect{
        viewportRect.origin.x,
        viewportRect.origin.y,
        viewportRect.size.x,
        viewportRect.size.y};
    result.viewportAllowsMouseInteraction = viewportRect.size.x > 0.0f && viewportRect.size.y > 0.0f;
    // While driving, R puts the car back on its wheels rather than switching the gizmo.
    if (!state.vehicleStatus.active)
    {
        HandleViewportShortcuts(scene, viewportRect);
    }
    RefreshViewportMatrices(camera, matrices, scene, result.viewportExtent, currentBackendType);
    if (state.vehicleStatus.active)
    {
        DrawVehiclePhysicsOverlay(
            *viewportRect.drawList,
            viewportRect.origin,
            viewportRect.size,
            matrices.projection * matrices.view,
            state.vehicleStatus.wheels,
            state.vehicle.overlay,
            UiScale());
        if (state.vehicle.overlay.enabled && state.vehicle.overlay.linkage)
        {
            DrawVehicleLinkageOverlay(
                *viewportRect.drawList, viewportRect.origin, viewportRect.size, matrices.projection * matrices.view, state.vehicleStatus.linkage, UiScale());
        }
    }
    if (state.vehicleRigStatus.active && context.windows.Get<SuspensionRigsPanel>().ShowLinkage())
    {
        DrawVehicleLinkageOverlay(
            *viewportRect.drawList, viewportRect.origin, viewportRect.size, matrices.projection * matrices.view, state.vehicleRigStatus.linkage, UiScale());
    }
    DrawVideoRecordingIndicator(viewportRect, UiScale(), state.videoRecording);
    // GT7's driving HUD along the bottom while a car is driven.
    const bool drivingHud = state.vehicleStatus.active && state.commands.drivingHud;
    if (drivingHud && viewportRect.drawList != nullptr)
    {
        DrawGt7Hud(*viewportRect.drawList, viewportRect.origin, viewportRect.size, BuildGt7HudInput(state.vehicleStatus, ImGui::GetTime()));
    }
    // Centred on the car while one is driven, else on the camera.
    if (state.vehicleStatus.active)
    {
        DrawMinimap(
            viewportRect,
            UiScale(),
            state.minimapTexture,
            scene.GetMinimap(),
            state.vehicleStatus.pose.position,
            state.vehicleStatus.pose.rotation * glm::vec3(0.0f, 0.0f, 1.0f),
            drivingHud);
    }
    else
    {
        DrawMinimap(viewportRect, UiScale(), state.minimapTexture, scene.GetMinimap(), camera.position, camera.GetForward());
    }
    if (fullscreen)
    {
        DrawFullscreenViewportHud(viewportRect, UiScale(), ImGui::GetTime() - state.fullscreenEnteredTime, state.vehicleStatus, drivingHud);
        return;
    }
    DrawViewManipulator(camera, matrices, viewportRect, UiScale());
    RefreshViewportMatrices(camera, matrices, scene, result.viewportExtent, currentBackendType);
    // View > Gizmos hides the transform gizmo and the lights' shapes; lights stay selectable.
    if (state.commands.gizmos)
    {
        DrawGizmoOverlay(scene, matrices, viewportRect, m_gizmoDragSnapState, UiScale());
        DrawLightGizmos(scene, matrices, viewportRect, UiScale());
    }
    std::vector<ProjectedEntityCenter> projectedCenters = ProjectSceneCenters(scene, matrices, viewportRect);
    AppendLightProjectedCenters(scene, matrices, viewportRect, UiScale(), projectedCenters);
    HandleViewportSelection(scene, projectedCenters, viewportRect, UiScale());
    DrawViewportSelectionOverlay(scene, matrices, viewportRect, UiScale(), static_cast<bool>(state.selectionOutlineTexture));
    const float textMargin = kOverlayTextMarginPixels * UiScale();
    ImGui::SetCursorScreenPos(ImVec2(viewportRect.origin.x + textMargin, viewportRect.origin.y + textMargin));
    ImGui::BeginGroup();
    ImGui::TextUnformatted("Viewport");
    ImGui::TextUnformatted("F to frame, R toggles combined/scale gizmo, right click deselects, drag assets here to place");
    ImGui::Text(
        "Render Size: %u x %u%s", result.viewportExtent.width, result.viewportExtent.height, fixedExtent.has_value() ? " (fixed)" : "");
    ImGui::Text("Viewport FPS: %.1f", ImGui::GetIO().Framerate);
    ImGui::EndGroup();
}
}
