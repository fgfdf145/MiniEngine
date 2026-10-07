#include "graphics_debug_panel.h"

#include <engine/editor/editor_ui.h>
#include <engine/editor/ui/editor_ui_internal.h>

#include <IconsPhosphor.h>
#include <imgui.h>

#include <array>

namespace me
{

GraphicsDebugPanel::GraphicsDebugPanel()
    : EditorPanel("graphics_debug", "Graphics Debug", ICON_PH_BUG, EditorDockSlot::RightBottom)
{
}

void GraphicsDebugPanel::OnGui(EditorContext& context)
{
    const EditorSharedState& state = context.state;
    RenderDebugSettings& debug = context.state.renderDebug;
    ImGui::Checkbox("Forward only (comparison)", &debug.forwardOnly);
    // Renders as the Khronos glTF Sample Viewer does by default: PBR Neutral, fixed exposure,
    // no glare, AO or SSR, and the viewer's camera framing whenever the scene or viewport changes.
    ImGui::Checkbox("Khronos reference view (comparison)", &debug.khronosReference);
    // The scene's share of the viewport's pixels: 100 % renders every display pixel. DLSS picks its
    // own render size from its quality mode.
    ImGui::BeginDisabled(state.DlssResolves());
    float renderScalePercent = debug.renderScale * 100.0f;
    if (ImGui::SliderFloat("Render scale", &renderScalePercent, 25.0f, 100.0f, "%.0f %%"))
    {
        debug.renderScale = renderScalePercent / 100.0f;
    }
    ImGui::EndDisabled();
    // NVIDIA DLSS replaces TAA in the deferred order: DLAA at the viewport's size, or super
    // resolution from a smaller render size. Where it cannot run, TAA resolves instead.
    static constexpr std::array<const char*, 6> kDlssModeNames = {
        "Off", "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance"};
    int dlssMode = static_cast<int>(debug.dlssMode);
    ImGui::BeginDisabled(!state.dlssAvailable || debug.forwardOnly);
    if (ImGui::Combo("DLSS", &dlssMode, kDlssModeNames.data(), static_cast<int>(kDlssModeNames.size())))
    {
        debug.dlssMode = static_cast<DlssMode>(dlssMode);
    }
    ImGui::EndDisabled();
    // The model DLSS runs. Default names the one DLSS picks for the current mode (SDK 310.9).
    const char* defaultPresetName = debug.dlssMode == DlssMode::Performance        ? "Default (M)"
                                    : debug.dlssMode == DlssMode::UltraPerformance ? "Default (L)"
                                                                                           : "Default (K)";
    const std::array<const char*, 5> dlssPresetNames = {defaultPresetName, "J", "K", "L", "M"};
    int dlssPreset = static_cast<int>(debug.dlssPreset);
    ImGui::BeginDisabled(!state.DlssResolves());
    if (ImGui::Combo("DLSS preset", &dlssPreset, dlssPresetNames.data(), static_cast<int>(dlssPresetNames.size())))
    {
        debug.dlssPreset = static_cast<DlssPreset>(dlssPreset);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(
            "K: transformer, best image quality at a higher cost; the default for DLAA, Quality and Balanced.\n"
            "J: like K, slightly less ghosting but more flicker.\n"
            "M: the default for Performance.  L: the default for Ultra Performance.");
    }
    // Ray reconstruction denoises the ray traced effects as DLSS upscales.
    ImGui::BeginDisabled(!state.DlssResolves() || !state.dlssRayReconstructionAvailable);
    ImGui::Checkbox("DLSS ray reconstruction", &debug.dlssRayReconstruction);
    ImGui::EndDisabled();
    if (!state.dlssAvailable || !state.dlssRayReconstructionAvailable)
    {
        ImGui::TextDisabled("DLSS: %s", state.dlssStatus.c_str());
    }
    if (!state.gpuMemoryStatus.empty())
    {
        ImGui::TextDisabled("%s", state.gpuMemoryStatus.c_str());
    }
    // Off, every pixel loops over every light: the path clustering must match pixel for pixel.
    ImGui::Checkbox("Clustered lighting", &debug.clusteredLighting);
    ImGui::Checkbox("Local light shadows", &debug.localLightShadows);
    // Off, or on a GPU without ray queries, rays walk the ray scene's hierarchies in compute: the
    // same hits, found more slowly.
    ImGui::Checkbox("Hardware ray tracing", &debug.hardwareRayTracing);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("DDGI probe rays use ray queries when the GPU has them (see the log); off uses the compute walk");
    }
    // What hardware ray tracing replaces: each effect falls back to its screen-space or shadow-map
    // counterpart while the switch above is off or the GPU has no ray queries.
    ImGui::BeginDisabled(!debug.hardwareRayTracing);
    RayTracingSettings& rayTracing = debug.rayTracing;
    ImGui::Indent();
    ImGui::Checkbox("Ray traced sun shadows", &rayTracing.sunShadows);
    ImGui::Checkbox("Ray traced local light shadows", &rayTracing.localShadows);
    ImGui::Checkbox("Ray traced reflections", &rayTracing.reflections);
    ImGui::Checkbox("Ray traced ambient occlusion", &rayTracing.ambientOcclusion);
    ImGui::Checkbox("DDGI probe occlusion", &rayTracing.probeOcclusion);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Darkens the probes' light where spaces are smaller than a coarse level's spacing");
    }
    DragIntInRange("Occlusion rays", &rayTracing.occlusionRays, 1, 8);
    ImGui::Checkbox("Denoise traced shadows", &rayTracing.denoise);
    if (ImGui::SmallButton("Reset##rt"))
    {
        rayTracing = RayTracingSettings{};
    }
    ImGui::Unindent();
    ImGui::EndDisabled();
    DragFloatInRange("Shadow distance (m)", &debug.shadowDistance, 10.0f, 5000.0f, "%.0f");
    // The forward-only order has no motion vectors, so TAA is off there whatever this says; DLSS
    // takes its place while it resolves.
    ImGui::BeginDisabled(state.DlssResolves());
    ImGui::Checkbox("Temporal anti-aliasing", &debug.taa);
    ImGui::EndDisabled();
    ImGui::Checkbox("Specular anti-aliasing", &debug.specularAntiAliasing);
    // Bloom needs no motion vectors, so unlike what follows it works in the forward-only order.
    ImGui::SeparatorText("Glare (bloom)");
    ImGui::Checkbox("Enabled##bloom", &debug.bloom.enabled);
    ImGui::BeginDisabled(!debug.bloom.enabled);
    // 1 is the diffraction the exposure's aperture produces; the Camera panel shows that aperture.
    DragFloatInRange("Strength##bloom", &debug.bloom.strength, 0.0f, 4.0f, "%.2f");
    ImGui::EndDisabled();

    // The trace needs TAA's history for its colour and the G-buffer, so it is off without TAA
    // and in the forward-only order.
    ImGui::SeparatorText("Screen-space reflections");
    ImGui::Checkbox("Enabled##ssr", &debug.ssr.enabled);
    ImGui::BeginDisabled(!debug.ssr.enabled);
    DragFloatInRange("Max roughness##ssr", &debug.ssr.maxRoughness, 0.05f, 1.0f, "%.2f");
    DragFloatInRange("Max distance (m)##ssr", &debug.ssr.maxDistance, 1.0f, 200.0f, "%.0f");
    ImGui::EndDisabled();

    ImGui::SeparatorText("Output");
    // HDR10 when the display offers it (GT7's HDR curve); the UI keeps its SDR brightness.
    ImGui::Checkbox("HDR output", &debug.hdrOutput);
    ImGui::BeginDisabled(!debug.hdrOutput);
    DragFloatInRange("Display peak (nits)", &debug.hdrPeakNits, 250.0f, 10000.0f, "%.0f");
    ImGui::EndDisabled();
    // Also Render > Tone Mapping. The Khronos reference view always uses PBR Neutral.
    static constexpr std::array<const char*, 3> kToneMapperNames = {"GT7", "PBR Neutral", "None (clipped)"};
    int toneMapper = static_cast<int>(debug.toneMapper);
    ImGui::BeginDisabled(debug.khronosReference);
    if (ImGui::Combo("Tone mapping", &toneMapper, kToneMapperNames.data(), static_cast<int>(kToneMapperNames.size())))
    {
        debug.toneMapper = static_cast<ToneMapper>(toneMapper);
    }
    ImGui::EndDisabled();
    // The forward-only order never writes the G-buffer, so there is nothing to view.
    ImGui::BeginDisabled(debug.forwardOnly);
    // Order matches GBufferDebugView's numeric values.
    static constexpr std::array<const char*, 19> kGBufferViewNames = {
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
        "DDGI: probes",
        "Ray traced sun shadow",
        "DDGI probe occlusion"};
    int gbufferView = static_cast<int>(debug.gbufferView);
    if (ImGui::Combo(
            "Viewport output",
            &gbufferView,
            kGBufferViewNames.data(),
            static_cast<int>(kGBufferViewNames.size())))
    {
        debug.gbufferView = static_cast<GBufferDebugView>(gbufferView);
    }

    // The forward-only order runs no AO pass, so these are disabled with the views above.
    ImGui::SeparatorText("Ambient occlusion");
    AoSettings& ao = debug.ao;
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
    GiSettings& gi = debug.gi;
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
    DdgiSettings& ddgi = debug.ddgi;
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
}
