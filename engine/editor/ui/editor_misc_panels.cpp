#include "editor_ui.h"

#include <material_graph_runtime.h>
#include <model_loader.h>
#include <texture_loader.h>

#include <editor_world.h>
#include <file_dialog/file_dialog.h>
#include <log/log.h>
#include <ui/ui_scale.h>
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

void EditorUiController::DrawCameraPanel(Camera& camera)
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
        ImGui::Text("Wheel: Fov (Speed while Right Mouse held)");
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
        ImGui::SliderFloat(
            "Fov",
            &camera.fovDegrees,
            WorldUnits::kUiCameraFovMinDegrees,
            WorldUnits::kUiCameraFovMaxDegrees
        );
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

void EditorUiController::DrawInputMonitorPanel()
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
