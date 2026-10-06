#include <engine/editor/editor_ui.h>
#include "editor_ui_internal.h"

#include <engine/core/log/log.h>
#include <engine/core/paths/engine_paths.h>
#include <engine/core/version/engine_version.h>
#include <engine/platform/ui/ui_scale.h>
#include <engine/renderer/glare.h>
#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <system_error>
#include <utility>
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
    ImGui::End();
}

void EditorUiController::DrawGraphicsDebugPanel()
{
    // The settings keep applying while this window is closed, as they did when they lived in the
    // Camera panel: closing a debug window hides its controls, it does not reset the renderer.
    if (ImGui::Begin("Graphics Debug", &m_showGraphicsDebugWindow))
    {
        ImGui::Checkbox("Forward only (comparison)", &m_renderDebug.forwardOnly);
        // Renders as the Khronos glTF Sample Viewer does by default: PBR Neutral, fixed exposure,
        // no glare, AO or SSR, and the viewer's camera framing whenever the scene or viewport changes.
        ImGui::Checkbox("Khronos reference view (comparison)", &m_renderDebug.khronosReference);
        // The scene's share of the viewport's pixels: 100 % renders every display pixel.
        float renderScalePercent = m_renderDebug.renderScale * 100.0f;
        if (ImGui::SliderFloat("Render scale", &renderScalePercent, 25.0f, 100.0f, "%.0f %%"))
        {
            m_renderDebug.renderScale = renderScalePercent / 100.0f;
        }
        // Off, every pixel loops over every light: the path clustering must match pixel for pixel.
        ImGui::Checkbox("Clustered lighting", &m_renderDebug.clusteredLighting);
        ImGui::Checkbox("Local light shadows", &m_renderDebug.localLightShadows);
        DragFloatInRange("Shadow distance (m)", &m_renderDebug.shadowDistance, 10.0f, 5000.0f, "%.0f");
        // The forward-only order has no motion vectors, so TAA is off there whatever this says.
        ImGui::Checkbox("Temporal anti-aliasing", &m_renderDebug.taa);
        ImGui::Checkbox("Specular anti-aliasing", &m_renderDebug.specularAntiAliasing);
        // Bloom needs no motion vectors, so unlike what follows it works in the forward-only order.
        ImGui::SeparatorText("Glare (bloom)");
        ImGui::Checkbox("Enabled##bloom", &m_renderDebug.bloom.enabled);
        ImGui::BeginDisabled(!m_renderDebug.bloom.enabled);
        // 1 is the diffraction the exposure's aperture produces; the Camera panel shows that aperture.
        DragFloatInRange("Strength##bloom", &m_renderDebug.bloom.strength, 0.0f, 4.0f, "%.2f");
        ImGui::EndDisabled();

        // The trace needs TAA's history for its colour and the G-buffer, so it is off without TAA
        // and in the forward-only order.
        ImGui::SeparatorText("Screen-space reflections");
        ImGui::Checkbox("Enabled##ssr", &m_renderDebug.ssr.enabled);
        ImGui::BeginDisabled(!m_renderDebug.ssr.enabled);
        DragFloatInRange("Max roughness##ssr", &m_renderDebug.ssr.maxRoughness, 0.05f, 1.0f, "%.2f");
        DragFloatInRange("Max distance (m)##ssr", &m_renderDebug.ssr.maxDistance, 1.0f, 200.0f, "%.0f");
        ImGui::EndDisabled();

        ImGui::SeparatorText("Output");
        // HDR10 when the display offers it (GT7's HDR curve); the UI keeps its SDR brightness.
        ImGui::Checkbox("HDR output", &m_renderDebug.hdrOutput);
        ImGui::BeginDisabled(!m_renderDebug.hdrOutput);
        DragFloatInRange("Display peak (nits)", &m_renderDebug.hdrPeakNits, 250.0f, 10000.0f, "%.0f");
        ImGui::EndDisabled();
        // Also Render > Tone Mapping. The Khronos reference view always uses PBR Neutral.
        static constexpr std::array<const char*, 3> kToneMapperNames = {"GT7", "PBR Neutral", "None (clipped)"};
        int toneMapper = static_cast<int>(m_renderDebug.toneMapper);
        ImGui::BeginDisabled(m_renderDebug.khronosReference);
        if (ImGui::Combo("Tone mapping", &toneMapper, kToneMapperNames.data(), static_cast<int>(kToneMapperNames.size())))
        {
            m_renderDebug.toneMapper = static_cast<ToneMapper>(toneMapper);
        }
        ImGui::EndDisabled();
        // The forward-only order never writes the G-buffer, so there is nothing to view.
        ImGui::BeginDisabled(m_renderDebug.forwardOnly);
        // Order matches GBufferDebugView's numeric values.
        static constexpr std::array<const char*, 17> kGBufferViewNames = {
            "Shaded",
            "G-buffer: albedo",
            "G-buffer: shading normal",
            "G-buffer: geometric normal",
            "G-buffer: metallic / roughness / occlusion",
            "G-buffer: emissive",
            "G-buffer: motion vectors",
            "Ambient occlusion",
            "Light clusters",
            "G-buffer: specular (sqrt F0)",
            "Screen-space reflections",
            "G-buffer: coat and anisotropy",
            "G-buffer: sheen",
            "Screen-space GI",
            "DDGI: ray-traced scene",
            "DDGI: irradiance",
            "DDGI: probes"};
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

        // One bounce of the lit image onto its neighbours, through the same bitmask as the AO.
        ImGui::SeparatorText("Screen-space GI");
        GiSettings& gi = m_renderDebug.gi;
        ImGui::Checkbox("Enabled##gi", &gi.enabled);
        ImGui::BeginDisabled(!gi.enabled);
        DragFloatInRange("Radius (m)##gi", &gi.radius, 0.25f, 10.0f, "%.2f");
        DragFloatInRange("Thickness (m)##gi", &gi.thickness, 0.01f, 2.0f, "%.2f");
        DragFloatInRange("Strength##gi", &gi.strength, 0.0f, 4.0f, "%.2f");
        DragIntInRange("Slices##gi", &gi.sliceCount, 1, 4);
        DragIntInRange("Steps##gi", &gi.stepCount, 2, 32);
        ImGui::Checkbox("Spatial filter##gi", &gi.spatialFilter);
        ImGui::Checkbox("Temporal filter##gi", &gi.temporalFilter);
        ImGui::EndDisabled();
        if (ImGui::SmallButton("Reset##gi"))
        {
            gi = GiSettings{};
        }
        ImGui::EndDisabled();

        // Probes around the camera that trace the scene: the diffuse ambient's light, off screen and
        // bounced any number of times. Forward-shaded surfaces use them too, so the forward-only
        // order keeps them.
        ImGui::SeparatorText("DDGI");
        DdgiSettings& ddgi = m_renderDebug.ddgi;
        ImGui::Checkbox("Enabled##ddgi", &ddgi.enabled);
        ImGui::BeginDisabled(!ddgi.enabled);
        DragIntInRange("Levels##ddgi", &ddgi.levels, 1, 4);
        DragFloatInRange("Spacing (m)##ddgi", &ddgi.baseSpacing, 0.25f, 8.0f, "%.2f");
        DragIntInRange("Probes per frame##ddgi", &ddgi.probesPerFrame, 64, 4096);
        DragFloatInRange("Hysteresis##ddgi", &ddgi.hysteresis, 0.0f, 0.999f, "%.3f");
        DragFloatInRange("Normal bias##ddgi", &ddgi.normalBias, 0.0f, 1.0f, "%.2f");
        DragFloatInRange("View bias##ddgi", &ddgi.viewBias, 0.0f, 1.0f, "%.2f");
        DragIntInRange("Probe view level##ddgi", &ddgi.probeViewLevel, 0, 3);
        ImGui::EndDisabled();
        if (ImGui::SmallButton("Reset##ddgi"))
        {
            ddgi = DdgiSettings{};
        }
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

void EditorUiController::FocusWindowWhenDrawn(std::string windowName)
{
    m_focusWindowRequest = std::move(windowName);
}

void EditorUiController::DrawPreferencesWindow()
{
    ImGui::SetNextWindowSize(ImVec2(420.0f * m_effectiveUiScale, 0.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Preferences", &m_showPreferencesWindow))
    {
        ImGui::SeparatorText("Interface");
        // The same setting as the Camera panel's, saved with the editor settings.
        if (DragFloatInRange("UI Scale Multiplier", &m_uiScale, 0.75f, 2.50f, "%.2f x"))
        {
            ApplyUiScale();
        }
        ImGui::Text("Effective UI Scale: %.2f x", m_effectiveUiScale);
        ImGui::SeparatorText("Theme");
        ImGui::TextUnformatted("The editor's colours are edited in the Theme window.");
        if (ImGui::Button("Open Theme"))
        {
            m_showThemeWindow = true;
            FocusWindowWhenDrawn("Theme");
        }
        ImGui::SeparatorText("Audio");
        float volumePercent = m_audio.masterVolume * 100.0f;
        ImGui::BeginDisabled(m_audio.muted);
        if (DragFloatInRange("Master Volume", &volumePercent, 0.0f, 100.0f, "%.0f %%"))
        {
            m_audio.masterVolume = volumePercent / 100.0f;
        }
        ImGui::EndDisabled();
        ImGui::Checkbox("Mute", &m_audio.muted);
        ImGui::TextDisabled("Output: %s", m_audioStatus.empty() ? "None" : m_audioStatus.c_str());
        ImGui::SeparatorText("Rendering");
        ImGui::TextUnformatted("Debug views, tone mapping and the render passes' switches are in");
        ImGui::TextUnformatted("the Graphics Debug window.");
        if (ImGui::Button("Open Graphics Debug"))
        {
            m_showGraphicsDebugWindow = true;
            FocusWindowWhenDrawn("Graphics Debug");
        }
    }
    ImGui::End();
}

void EditorUiController::OpenDocumentation()
{
    const std::filesystem::path readme = EnginePaths::ProjectRoot() / "README.md";
    std::error_code error;
    if (!std::filesystem::exists(readme, error))
    {
        LOG_WARN("No documentation at {}", readme.string());
        return;
    }
    // A file URL: three slashes before a drive letter, two before an absolute POSIX path. Spaces are
    // the one character project paths commonly hold that a URL cannot.
    std::string path = std::filesystem::absolute(readme, error).generic_string();
    std::string url = path.starts_with('/') ? "file://" : "file:///";
    for (const char character : path)
    {
        url += character == ' ' ? std::string("%20") : std::string(1, character);
    }
    if (!SDL_OpenURL(url.c_str()))
    {
        LOG_WARN("Could not open {}: {}", url, SDL_GetError());
    }
}

void EditorUiController::DrawHelpWindows(bool fullscreen)
{
    // The About modal also shows over the fullscreen viewport; the shortcuts window waits for it to end.
    if (!fullscreen && m_showKeyboardShortcutsWindow)
    {
        // Keys the viewport handles itself, not through a command.
        static constexpr std::array<std::pair<const char*, const char*>, 6> kViewportKeys = {{
            {"W A S D", "Move the camera"},
            {"Right mouse", "Look around"},
            {"Alt + Right mouse", "Orbit the selection"},
            {"Middle mouse", "Pan"},
            {"R", "Toggle the combined and scale gizmo"},
            {"Escape", "Leave the fullscreen viewport"},
        }};
        ImGui::SetNextWindowSize(ImVec2(460.0f * m_effectiveUiScale, 520.0f * m_effectiveUiScale), ImGuiCond_FirstUseEver);
        DrawKeyboardShortcutsWindow(m_commands, &m_showKeyboardShortcutsWindow, kViewportKeys);
    }

    constexpr const char* kAboutTitle = "About MiniEngine";
    if (std::exchange(m_openAboutModal, false))
    {
        ImGui::OpenPopup(kAboutTitle);
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kAboutTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("MiniEngine %s", EngineVersion::String());
        ImGui::TextUnformatted("A Vulkan renderer and scene editor.");
        ImGui::Separator();
        const int sdlVersion = SDL_GetVersion();
        ImGui::TextDisabled(
            "Dear ImGui %s, SDL %d.%d.%d",
            ImGui::GetVersion(),
            SDL_VERSIONNUM_MAJOR(sdlVersion),
            SDL_VERSIONNUM_MINOR(sdlVersion),
            SDL_VERSIONNUM_MICRO(sdlVersion));
        ImGui::TextDisabled("Project folder: %s", EnginePaths::ProjectRoot().string().c_str());
        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(120.0f * m_effectiveUiScale, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
}
