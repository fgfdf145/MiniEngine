#include "camera_panel.h"

#include <engine/editor/editor_ui.h>
#include <engine/editor/ui/editor_ui_internal.h>
#include <engine/platform/ui/ui_scale.h>
#include <engine/renderer/glare.h>

#include <IconsPhosphor.h>
#include <imgui.h>

#include <algorithm>

namespace me
{

CameraPanel::CameraPanel()
    : EditorPanel("camera", "Camera", ICON_PH_VIDEO_CAMERA, EditorDockSlot::RightBottom)
{
    Open();
}

void CameraPanel::OnGui(EditorContext& context)
{
    Camera& camera = context.camera;
    if (DragFloatInRange("UI Scale Multiplier", &context.style.UiScaleMultiplier(), 0.75f, 2.50f, "%.2f x"))
    {
        context.style.ApplyUiScale();
    }
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("Platform UI Profile: %s", platform::ui::GetCurrentOperatingSystemName());
    ImGui::Text("Window DPI Scale: %.2f x", context.style.WindowDpiScale());
    ImGui::Text("Effective UI Scale: %.2f x", context.style.EffectiveUiScale());
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
    // The aperture the glare is diffracted through, chosen by the exposure (see glare.h).
    ImGui::Text("Glare aperture f/%.1f", GlareFNumberFromEv100(camera.exposureEv100));

    // GT7's auto white balance (see white_balance.h): partial adaptation to the scene's light.
    ImGui::Checkbox("Auto White Balance", &camera.autoWhiteBalance.enabled);
    ImGui::BeginDisabled(!camera.autoWhiteBalance.enabled);
    DragFloatInRange("Adaptation Degree", &camera.autoWhiteBalance.degree, 0.0f, 1.0f, "%.2f");
    DragFloatInRange("Target White (K)", &camera.autoWhiteBalance.targetKelvin, kMinWhiteBalanceTargetKelvin,
                     kMaxWhiteBalanceTargetKelvin, "%.0f");
    ImGui::Text("Adapted white %.0f K", camera.adaptedWhiteKelvin);
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
        // The long-term stage follows the frame, the sun and the sky; the view stays within the
        // short-term range of it (see StepAutoExposure).
        ImGui::Text("Long-term adaptation EV100 %.2f", camera.adaptedLongTermEv100);
        DragFloatInRange("Short-term Range (EV)", &autoExposure.shortTermRangeEv, 0.0f, 10.0f, "%.1f");
        if (ImGui::SmallButton("Reset##autoexposure"))
        {
            autoExposure = AutoExposureSettings{};
        }
    }
}
}
