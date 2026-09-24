#include <engine/editor/editor_ui.h>
#include "editor_ui_internal.h"

#include <engine/core/log/log.h>
#include <engine/platform/ui/ui_scale.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <vector>

namespace me
{

void EditorUiController::DrawCameraPanel(Camera& camera)
{
    if (ImGui::Begin("Camera", &m_showCameraWindow))
    {
        if (DragFloatInRange("UI Scale Multiplier", &m_uiScale, 0.75f, 2.50f, "%.2f x"))
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
        ImGui::Text("Wheel: Fov (Speed while Right Mouse held)");
        DragFloat3InRange(
            "Position (m)",
            &camera.position.x,
            -WorldUnits::kUiCameraPositionRangeMeters,
            WorldUnits::kUiCameraPositionRangeMeters,
            "%.2f",
            0.05f);
        DragFloatInRange("Yaw", &camera.yawDegrees, -180.0f, 180.0f, "%.1f", 0.5f);
        DragFloatInRange("Pitch", &camera.pitchDegrees, -89.0f, 89.0f, "%.1f", 0.5f);
        DragFloatInRange(
            "Speed (m/s)",
            &camera.moveSpeed,
            WorldUnits::kUiCameraMoveSpeedMinMetersPerSecond,
            WorldUnits::kUiCameraMoveSpeedMaxMetersPerSecond,
            "%.2f");
        DragFloatInRange("Sensitivity", &camera.mouseSensitivity, 0.01f, 1.0f);
        DragFloatInRange(
            "Fov",
            &camera.fovDegrees,
            WorldUnits::kUiCameraFovMinDegrees,
            WorldUnits::kUiCameraFovMaxDegrees,
            "%.1f");
        DragFloatInRange(
            "Near (m)",
            &camera.nearPlane,
            WorldUnits::kUiCameraNearMinMeters,
            WorldUnits::kUiCameraNearMaxMeters,
            "%.3f",
            0.005f);
        DragFloatInRange(
            "Far (m)",
            &camera.farPlane,
            WorldUnits::kUiCameraFarMinMeters,
            WorldUnits::kUiCameraFarMaxMeters,
            "%.1f");
        ImGui::Separator();
        AutoExposureSettings& autoExposure = camera.autoExposure;
        ImGui::Checkbox("Auto Exposure", &autoExposure.enabled);

        // In auto mode the renderer writes exposureEv100 every frame, so the field only shows it.
        ImGui::BeginDisabled(autoExposure.enabled);
        DragFloatInRange(
            "Exposure (EV100)",
            &camera.exposureEv100,
            kMinExposureEv100,
            kMaxExposureEv100,
            "%.2f");
        camera.exposureEv100 = std::clamp(camera.exposureEv100, kMinExposureEv100, kMaxExposureEv100);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##exposure"))
        {
            camera.exposureEv100 = kDefaultExposureEv100;
        }
        ImGui::EndDisabled();

        if (autoExposure.enabled)
        {
            DragFloatInRange("Compensation (EV)", &autoExposure.compensationEv, -5.0f, 5.0f, "%+.1f");
            ImGui::DragFloatRange2(
                "EV100 Range",
                &autoExposure.minEv100,
                &autoExposure.maxEv100,
                0.1f,
                kMinExposureEv100,
                kMaxExposureEv100,
                "Min %.1f",
                "Max %.1f",
                ImGuiSliderFlags_AlwaysClamp);
            DragFloatInRange(
                "Adapt to Brighter (1/s)",
                &autoExposure.adaptToBrighterPerSecond,
                0.1f,
                10.0f,
                "%.1f");
            DragFloatInRange(
                "Adapt to Darker (1/s)",
                &autoExposure.adaptToDarkerPerSecond,
                0.1f,
                10.0f,
                "%.1f");
            if (ImGui::SmallButton("Reset##autoexposure"))
            {
                autoExposure = AutoExposureSettings{};
            }
        }
    }
    ImGui::End();
}

void EditorUiController::DrawGraphicsDebugPanel()
{
    // The settings keep applying while this window is closed, as they did when they lived in the
    // Camera panel: closing a debug window hides its controls, it does not reset the renderer.
    if (ImGui::Begin("Graphics Debug", &m_showGraphicsDebugWindow))
    {
        ImGui::Checkbox("Forward only (comparison)", &m_renderDebug.forwardOnly);
        // Off, every pixel loops over every light: the path clustering must match pixel for pixel.
        ImGui::Checkbox("Clustered lighting", &m_renderDebug.clusteredLighting);
        // The forward-only order never writes the G-buffer, so there is nothing to view.
        ImGui::BeginDisabled(m_renderDebug.forwardOnly);
        // Order matches GBufferDebugView's numeric values.
        static constexpr std::array<const char*, 10> kGBufferViewNames = {
            "Shaded",
            "G-buffer: albedo",
            "G-buffer: shading normal",
            "G-buffer: geometric normal",
            "G-buffer: metallic / roughness / occlusion",
            "G-buffer: emissive",
            "G-buffer: motion vectors",
            "Ambient occlusion",
            "Light clusters",
            "G-buffer: custom data (clearcoat)"};
        int gbufferView = static_cast<int>(m_renderDebug.gbufferView);
        if (ImGui::Combo(
                "Viewport output",
                &gbufferView,
                kGBufferViewNames.data(),
                static_cast<int>(kGBufferViewNames.size())))
        {
            m_renderDebug.gbufferView = static_cast<GBufferDebugView>(gbufferView);
        }

        // The forward-only order runs no AO pass, so these are disabled with the views above.
        ImGui::SeparatorText("Ambient occlusion");
        AoSettings& ao = m_renderDebug.ao;
        ImGui::Checkbox("Enabled##ao", &ao.enabled);
        ImGui::BeginDisabled(!ao.enabled);
        DragFloatInRange("Radius (m)", &ao.radius, 0.1f, 5.0f, "%.2f");
        DragFloatInRange("Thickness (m)", &ao.thickness, 0.01f, 2.0f, "%.2f");
        DragIntInRange("Slices", &ao.sliceCount, 1, 4);
        DragIntInRange("Steps", &ao.stepCount, 2, 16);
        ImGui::Checkbox("Spatial filter", &ao.spatialFilter);
        ImGui::Checkbox("Temporal filter", &ao.temporalFilter);
        ImGui::EndDisabled();
        if (ImGui::SmallButton("Reset##ao"))
        {
            ao = AoSettings{};
        }
        ImGui::EndDisabled();
    }
    ImGui::End();
}

void EditorUiController::DrawInputMonitorPanel()
{
    ImGui::SetNextWindowSize(
        ImVec2(720.0f * m_effectiveUiScale, 360.0f * m_effectiveUiScale),
        ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Input Monitor", &m_showInputMonitorWindow))
    {
        Log::RefreshInputMessagesSnapshot(m_inputMonitorMessages, m_inputMonitorMessagesRevision);
        const std::vector<std::string>& inputMessages = m_inputMonitorMessages;
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
            // Only the visible lines are submitted.
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(inputMessages.size()));
            while (clipper.Step())
            {
                for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line)
                {
                    const std::string& message = inputMessages[static_cast<size_t>(line)];
                    ImGui::TextUnformatted(message.data(), message.data() + message.size());
                }
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
}
