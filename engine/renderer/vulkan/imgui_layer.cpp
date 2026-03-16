#include "imgui_layer.h"

#include "../imgui/imgui_impl_sdl3.h"
#include "../imgui/imgui_impl_vulkan.h"

#include <editor_scene.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <file_dialog/file_dialog.h>
#include <glm/common.hpp>
#include <glm/ext/matrix_clip_space.hpp>
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
constexpr uint32_t kImGuiDescriptorCount = 128;
constexpr ImU32 kSelectionOutlineColor = IM_COL32(255, 196, 64, 255);
constexpr ImGuiDockNodeFlags kEditorDockspaceFlags = ImGuiDockNodeFlags_PassthruCentralNode;

struct ProjectedEntityBounds
{
    entt::entity entity = entt::null;
    ImVec2 min{ 0.0f, 0.0f };
    ImVec2 max{ 0.0f, 0.0f };
    float depth = std::numeric_limits<float>::max();
    bool visible = false;
};

struct ViewportOverlayRect
{
    ImVec2 origin{ 0.0f, 0.0f };
    ImVec2 size{ 0.0f, 0.0f };
    ImDrawList* drawList = nullptr;
    bool hovered = false;
    bool focused = false;
};

ViewportOverlayRect BuildViewportOverlayRect(ImTextureID viewportTextureId)
{
    ViewportOverlayRect rect{};
    rect.drawList = ImGui::GetWindowDrawList();

    ImVec2 available = ImGui::GetContentRegionAvail();
    available.x = std::max(available.x, 1.0f);
    available.y = std::max(available.y, 1.0f);

    if (viewportTextureId)
    {
        ImGui::Image(viewportTextureId, available);
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

VkExtent2D BuildViewportExtent(const ViewportOverlayRect& rect)
{
    return VkExtent2D{
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

std::vector<ProjectedEntityBounds> ProjectSceneBounds(
    const EditorScene& scene,
    const ViewportMatrices& matrices,
    const ViewportOverlayRect& viewportRect
)
{
    std::vector<ProjectedEntityBounds> projectedBounds;
    if (viewportRect.size.x <= 0.0f || viewportRect.size.y <= 0.0f)
    {
        return projectedBounds;
    }

    const glm::mat4 viewProjection = matrices.projection * matrices.view;
    projectedBounds.reserve(scene.GetEntityOrder().size());

    scene.ForEachEntity([&](entt::entity entity, const TagComponent&, const TransformComponent&, const ModelComponent& model)
    {
        if (!model.hasBounds)
        {
            return;
        }

        ProjectedEntityBounds projected{};
        projected.entity = entity;

        float minX = std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float maxY = std::numeric_limits<float>::lowest();
        float depthAccumulation = 0.0f;
        int depthSamples = 0;

        const glm::mat4 modelMatrix = scene.GetModelMatrix(entity);
        for (const glm::vec3& corner : BuildBoundsCorners(model.minBounds, model.maxBounds))
        {
            const glm::vec4 clip = viewProjection * modelMatrix * glm::vec4(corner, 1.0f);
            if (clip.w <= 0.0f)
            {
                continue;
            }

            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (ndc.z < 0.0f || ndc.z > 1.0f)
            {
                continue;
            }

            const float screenX = viewportRect.origin.x + (ndc.x * 0.5f + 0.5f) * viewportRect.size.x;
            const float screenY = viewportRect.origin.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * viewportRect.size.y;
            minX = std::min(minX, screenX);
            minY = std::min(minY, screenY);
            maxX = std::max(maxX, screenX);
            maxY = std::max(maxY, screenY);
            depthAccumulation += ndc.z;
            ++depthSamples;
        }

        if (depthSamples == 0)
        {
            return;
        }

        projected.min = ImVec2(
            glm::clamp(minX, viewportRect.origin.x, viewportRect.origin.x + viewportRect.size.x),
            glm::clamp(minY, viewportRect.origin.y, viewportRect.origin.y + viewportRect.size.y)
        );
        projected.max = ImVec2(
            glm::clamp(maxX, viewportRect.origin.x, viewportRect.origin.x + viewportRect.size.x),
            glm::clamp(maxY, viewportRect.origin.y, viewportRect.origin.y + viewportRect.size.y)
        );
        projected.depth = depthAccumulation / static_cast<float>(depthSamples);
        projected.visible = projected.max.x > projected.min.x && projected.max.y > projected.min.y;
        if (projected.visible)
        {
            projectedBounds.push_back(projected);
        }
    });

    return projectedBounds;
}

const ProjectedEntityBounds* FindProjectedBounds(const std::vector<ProjectedEntityBounds>& projectedBounds, entt::entity entity)
{
    for (const ProjectedEntityBounds& bounds : projectedBounds)
    {
        if (bounds.entity == entity)
        {
            return &bounds;
        }
    }

    return nullptr;
}

entt::entity PickHoveredEntity(const std::vector<ProjectedEntityBounds>& projectedBounds)
{
    const ImVec2 mousePosition = ImGui::GetMousePos();
    entt::entity hoveredEntity = entt::null;
    float bestDepth = std::numeric_limits<float>::max();

    for (const ProjectedEntityBounds& bounds : projectedBounds)
    {
        const bool insideBounds =
            mousePosition.x >= bounds.min.x &&
            mousePosition.x <= bounds.max.x &&
            mousePosition.y >= bounds.min.y &&
            mousePosition.y <= bounds.max.y;
        if (!insideBounds || bounds.depth >= bestDepth)
        {
            continue;
        }

        bestDepth = bounds.depth;
        hoveredEntity = bounds.entity;
    }

    return hoveredEntity;
}

void DrawViewportSelectionOverlay(const EditorScene& scene, const std::vector<ProjectedEntityBounds>& projectedBounds)
{
    if (!scene.HasSelection())
    {
        return;
    }

    const ProjectedEntityBounds* bounds = FindProjectedBounds(projectedBounds, scene.GetSelectedEntity());
    if (bounds == nullptr)
    {
        return;
    }

    ImGui::GetWindowDrawList()->AddRect(bounds->min, bounds->max, kSelectionOutlineColor, 0.0f, 0, 2.0f);
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
    ImGui::DragFloat3("Scale (1 = source meters)", glm::value_ptr(transform.scale), 0.02f, WorldUnits::kMinimumScale, WorldUnits::kUiTransformScaleMax, "%.3f");
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

void RefreshViewportMatrices(Camera& camera, ViewportMatrices& matrices, const EditorScene& scene, VkExtent2D viewportExtent)
{
    matrices.view = camera.GetViewMatrix();
    matrices.projection = camera.GetProjectionMatrix(viewportExtent, false);
    matrices.renderProjection = camera.GetProjectionMatrix(viewportExtent, true);
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
    const std::vector<ProjectedEntityBounds>& projectedBounds,
    const ViewportOverlayRect& viewportRect
)
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse || !viewportRect.hovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        return;
    }

    if (scene.HasSelection() && (ImGuizmo::IsOver() || ImGuizmo::IsUsing()))
    {
        return;
    }

    scene.SetSelectedEntity(PickHoveredEntity(projectedBounds));
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

VulkanImGuiLayer::VulkanImGuiLayer(
    SDL_Window* window,
    VkInstance instance,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    uint32_t graphicsQueueFamily,
    VkQueue graphicsQueue
)
    : m_window(window),
      m_instance(instance),
      m_physicalDevice(physicalDevice),
      m_device(device),
      m_graphicsQueueFamily(graphicsQueueFamily),
      m_graphicsQueue(graphicsQueue)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    m_baseStyle = ImGui::GetStyle();
    ApplyUiScale();

    CreateDescriptorPool();

    if (!ImGui_ImplSDL3_InitForVulkan(m_window))
    {
        throw std::runtime_error("Failed to initialize ImGui SDL3 backend");
    }
}

VulkanImGuiLayer::~VulkanImGuiLayer()
{
    DestroyVulkanResources();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    }
}

void VulkanImGuiLayer::ProcessEvent(const SDL_Event& event)
{
    ImGui_ImplSDL3_ProcessEvent(&event);
}

void VulkanImGuiLayer::BeginFrame()
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ApplyUiScale();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
}

EditorUiFrameResult VulkanImGuiLayer::DrawEditorUi(
    Camera& camera,
    ViewportMatrices& matrices,
    EditorScene& scene,
    const std::string& currentModelPath,
    const std::string& lastLoadError,
    const std::string& lastSceneIoError,
    ImTextureID viewportTextureId,
    VkExtent2D viewportExtent
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
    ImGui::SliderFloat3("Position (m)", &camera.position.x, -WorldUnits::kUiCameraPositionRangeMeters, WorldUnits::kUiCameraPositionRangeMeters, "%.2f");
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
    ImGui::SliderFloat("Near (m)", &camera.nearPlane, WorldUnits::kUiCameraNearMinMeters, WorldUnits::kUiCameraNearMaxMeters, "%.3f");
    ImGui::SliderFloat("Far (m)", &camera.farPlane, WorldUnits::kUiCameraFarMinMeters, WorldUnits::kUiCameraFarMaxMeters, "%.1f");
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
    ImGui::TextWrapped("Selection: %s", scene.HasSelection() ? "Selected in viewport" : "Click a cube in the viewport to select");
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

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("Viewport");
    const ViewportOverlayRect viewportRect = BuildViewportOverlayRect(viewportTextureId);
    DrawViewportOverlay(viewportRect, viewportTextureId);
    result.viewportExtent = BuildViewportExtent(viewportRect);
    HandleViewportShortcuts(scene, camera, viewportRect);
    RefreshViewportMatrices(camera, matrices, scene, result.viewportExtent);
    DrawViewManipulator(camera, matrices, viewportRect);
    RefreshViewportMatrices(camera, matrices, scene, result.viewportExtent);
    DrawGizmoOverlay(scene, matrices, viewportRect);
    const std::vector<ProjectedEntityBounds> projectedBounds = ProjectSceneBounds(scene, matrices, viewportRect);
    HandleViewportSelection(scene, projectedBounds, viewportRect);
    DrawViewportSelectionOverlay(scene, projectedBounds);
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

ImDrawData* VulkanImGuiLayer::GetDrawData() const
{
    return ImGui::GetDrawData();
}

bool VulkanImGuiLayer::WantsKeyboardCapture() const
{
    return ImGui::GetIO().WantCaptureKeyboard;
}

bool VulkanImGuiLayer::WantsMouseCapture() const
{
    return ImGui::GetIO().WantCaptureMouse;
}

void VulkanImGuiLayer::ApplyUiScale()
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

float VulkanImGuiLayer::GetWindowUiScale() const
{
    const float displayScale = SDL_GetWindowDisplayScale(m_window);
    if (displayScale > 0.0f)
    {
        return displayScale;
    }

    const float pixelDensity = SDL_GetWindowPixelDensity(m_window);
    return pixelDensity > 0.0f ? pixelDensity : 1.0f;
}

void VulkanImGuiLayer::CreateOrUpdateVulkanResources(VkRenderPass renderPass, uint32_t imageCount)
{
    DestroyVulkanResources();

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = VK_API_VERSION_1_3;
    initInfo.Instance = m_instance;
    initInfo.PhysicalDevice = m_physicalDevice;
    initInfo.Device = m_device;
    initInfo.QueueFamily = m_graphicsQueueFamily;
    initInfo.Queue = m_graphicsQueue;
    initInfo.DescriptorPool = m_descriptorPool;
    initInfo.RenderPass = renderPass;
    initInfo.MinImageCount = imageCount;
    initInfo.ImageCount = imageCount;
    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.CheckVkResultFn = &VulkanImGuiLayer::CheckVkResult;

    if (!ImGui_ImplVulkan_Init(&initInfo))
    {
        throw std::runtime_error("Failed to initialize ImGui Vulkan backend");
    }

    UploadFonts();
    m_vulkanBackendInitialized = true;
}

void VulkanImGuiLayer::DestroyVulkanResources()
{
    if (m_vulkanBackendInitialized)
    {
        vkDeviceWaitIdle(m_device);
        ImGui_ImplVulkan_Shutdown();
        m_vulkanBackendInitialized = false;
    }
}

void VulkanImGuiLayer::CreateDescriptorPool()
{
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = kImGuiDescriptorCount;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = kImGuiDescriptorCount;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;

    CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "Failed to create ImGui descriptor pool");
}

void VulkanImGuiLayer::UploadFonts() const
{
    if (!ImGui_ImplVulkan_CreateFontsTexture())
    {
        throw std::runtime_error("Failed to upload ImGui font texture");
    }
}

void VulkanImGuiLayer::CheckVkResult(VkResult result)
{
    CheckVulkan(result, "ImGui Vulkan backend call failed");
}
