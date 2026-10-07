#include "graphics_debug_panel.h"

#include <engine/editor/editor_ui.h>
#include <engine/editor/ui/editor_ui_internal.h>
#include <engine/renderer/render_features.h>

#include <IconsPhosphor.h>
#include <imgui.h>

#include <array>
#include <optional>

namespace me
{

namespace
{
// What the backend reported it can run. The ray scene counts as ready: it comes and goes with
// streaming, and the controls should not flicker with it.
RenderCapabilities PanelCapabilities(const EditorSharedState& state)
{
    RenderCapabilities capabilities;
    capabilities.rayQueries = state.pathTracingAvailable;
    capabilities.raySceneReady = true;
    capabilities.pathTracer = state.pathTracingAvailable;
    capabilities.restirPt = state.pathTracingAvailable;
    capabilities.dlss = state.DlssResolves();
    capabilities.dlssRayReconstruction = state.DlssResolves() && state.dlssRayReconstructionAvailable && state.renderDebug.dlssRayReconstruction;
    return capabilities;
}

// Why the passes the deferred order adds for the ambient light (AO, GI, reflections) do not run.
const char* AmbientOffReason(const RenderDebugSettings& debug, const RenderFeatures& features)
{
    if (debug.khronosReference)
    {
        return "Off in the Khronos reference view";
    }
    if (!features.deferred)
    {
        return "Off in the forward-only order";
    }
    if (features.pathTracing)
    {
        return "Path tracing replaces it";
    }
    return nullptr;
}

// Why the ray traced effects do not run, or null where they can.
const char* RayTracingOffReason(const EditorSharedState& state, const RenderFeatures& features)
{
    if (features.rayTracedEffects)
    {
        return nullptr;
    }
    if (!state.pathTracingAvailable)
    {
        return "Needs a GPU with ray queries";
    }
    if (!state.renderDebug.hardwareRayTracing)
    {
        return "Hardware ray tracing is switched off";
    }
    return state.renderDebug.khronosReference ? "Off in the Khronos reference view" : "Off in the forward-only order";
}

// The line under a section whose pass the pipeline does not run, saying why.
void OffNote(bool runs, const char* reason)
{
    if (!runs && reason != nullptr)
    {
        ImGui::TextDisabled("%s", reason);
    }
}

// A switch for something the pipeline has no place for is greyed out and says why on hover.
bool PipelineCheckbox(const char* label, bool* value, bool runs, const char* reason)
{
    ImGui::BeginDisabled(!runs);
    const bool changed = ImGui::Checkbox(label, value);
    ImGui::EndDisabled();
    if (!runs && reason != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("%s", reason);
    }
    return changed;
}

// The viewport's output resolution: the panel's size, or a fixed one that does not change with the
// editor's layout or the window (shown at its own aspect, with bars round it).
void DrawViewportResolution(ViewportResolutionSettings& resolution, const std::optional<RenderExtent>& forced)
{
    struct Preset
    {
        const char* name;
        int width;
        int height;
    };
    static constexpr std::array<Preset, 7> kPresets = {{
        {"1280 x 720", 1280, 720},
        {"1600 x 900", 1600, 900},
        {"1920 x 1080", 1920, 1080},
        {"2560 x 1080", 2560, 1080},
        {"2560 x 1440", 2560, 1440},
        {"3440 x 1440", 3440, 1440},
        {"3840 x 2160", 3840, 2160},
    }};
    const auto presetIndex = [&]() -> int
    {
        for (size_t i = 0; i < kPresets.size(); ++i)
        {
            if (kPresets[i].width == resolution.width && kPresets[i].height == resolution.height)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    const int customItem = static_cast<int>(kPresets.size()) + 1;
    // Item 0 follows the panel, then the presets, then Custom.
    static bool customChosen = false;
    const int found = presetIndex();
    int item = !resolution.fixed ? 0 : (found >= 0 && !customChosen ? found + 1 : customItem);
    std::array<const char*, kPresets.size() + 2> names{};
    names[0] = "Fit viewport panel";
    for (size_t i = 0; i < kPresets.size(); ++i)
    {
        names[i + 1] = kPresets[i].name;
    }
    names[customItem] = "Custom";

    ImGui::BeginDisabled(forced.has_value());
    if (ImGui::Combo("Viewport resolution", &item, names.data(), static_cast<int>(names.size())))
    {
        resolution.fixed = item != 0;
        customChosen = item == customItem;
        if (item > 0 && item < customItem)
        {
            resolution.width = kPresets[static_cast<size_t>(item - 1)].width;
            resolution.height = kPresets[static_cast<size_t>(item - 1)].height;
        }
    }
    if (resolution.fixed && item == customItem)
    {
        int size[2] = {resolution.width, resolution.height};
        if (ImGui::InputInt2("Width x height", size, ImGuiInputTextFlags_EnterReturnsTrue))
        {
            resolution.width = std::clamp(size[0], ViewportResolutionSettings::kMinSize, ViewportResolutionSettings::kMaxSize);
            resolution.height = std::clamp(size[1], ViewportResolutionSettings::kMinSize, ViewportResolutionSettings::kMaxSize);
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(
            "Fit: the scene renders at the viewport panel's size and follows it.\n"
            "A fixed resolution renders at that size whatever the panel's or the window's size,\n"
            "shown whole at its own aspect. The render scale and DLSS apply to it as to the panel's size.");
    }
    if (forced.has_value())
    {
        ImGui::TextDisabled("Fixed at %u x %u by --viewport-size or a recording", forced->width, forced->height);
    }
}
}

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
    // What the pipeline these settings make runs (render_features.h, as the renderer decides it): a
    // control for anything it leaves out is greyed out. Resolved again after each switch that
    // changes the pipeline, so the controls below it follow within the frame.
    RenderFeatures features = ResolveRenderFeatures(debug, PanelCapabilities(state));
    const auto refresh = [&]
    {
        features = ResolveRenderFeatures(debug, PanelCapabilities(state));
    };
    const char* const khronosReason = "Off in the Khronos reference view";
    // The scene's share of the viewport's pixels: 100 % renders every display pixel. DLSS picks its
    // own render size from its quality mode.
    ImGui::BeginDisabled(!features.renderScale);
    float renderScalePercent = debug.renderScale * 100.0f;
    if (ImGui::SliderFloat("Render scale", &renderScalePercent, 25.0f, 100.0f, "%.0f %%"))
    {
        debug.renderScale = renderScalePercent / 100.0f;
    }
    ImGui::EndDisabled();
    DrawViewportResolution(debug.viewportResolution, state.forcedViewportExtent);
    // NVIDIA DLSS replaces TAA in the deferred order: DLAA at the viewport's size, or super
    // resolution from a smaller render size. Where it cannot run, TAA resolves instead.
    static constexpr std::array<const char*, 6> kDlssModeNames = {
        "Off", "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance"};
    int dlssMode = static_cast<int>(debug.dlssMode);
    ImGui::BeginDisabled(!state.dlssAvailable || debug.forwardOnly);
    if (ImGui::Combo("DLSS", &dlssMode, kDlssModeNames.data(), static_cast<int>(kDlssModeNames.size())))
    {
        debug.dlssMode = static_cast<DlssMode>(dlssMode);
        refresh();
    }
    ImGui::EndDisabled();
    // The model DLSS runs. Default names the one DLSS picks for the current mode (SDK 310.9).
    const char* defaultPresetName = debug.dlssMode == DlssMode::Performance        ? "Default (M)"
                                    : debug.dlssMode == DlssMode::UltraPerformance ? "Default (L)"
                                                                                           : "Default (K)";
    const std::array<const char*, 5> dlssPresetNames = {defaultPresetName, "J", "K", "L", "M"};
    int dlssPreset = static_cast<int>(debug.dlssPreset);
    ImGui::BeginDisabled(!features.dlssPreset);
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
    if (ImGui::Checkbox("DLSS ray reconstruction", &debug.dlssRayReconstruction))
    {
        refresh();
    }
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
    if (PipelineCheckbox("Local light shadows", &debug.localLightShadows, features.localLightShadows, khronosReason))
    {
        refresh();
    }
    // Off, or on a GPU without ray queries, rays walk the ray scene's hierarchies in compute: the
    // same hits, found more slowly.
    ImGui::BeginDisabled(!state.pathTracingAvailable);
    if (ImGui::Checkbox("Hardware ray tracing", &debug.hardwareRayTracing))
    {
        refresh();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip(state.pathTracingAvailable
                              ? "DDGI probe rays use ray queries when the GPU has them (see the log); off uses the compute walk"
                              : "The GPU has no ray queries: rays use the compute walk");
    }
    // What hardware ray tracing replaces: each effect falls back to its screen-space or shadow-map
    // counterpart where it does not run. Path tracing stands in for the ambient ones (and, as ReSTIR
    // PT, for the traced shadows too).
    const char* const rayTracingReason = RayTracingOffReason(state, features);
    const char* const pathTracedReason = rayTracingReason != nullptr ? rayTracingReason : "Path tracing replaces it";
    RayTracingSettings& rayTracing = debug.rayTracing;
    ImGui::Indent();
    PipelineCheckbox("Ray traced sun shadows", &rayTracing.sunShadows, features.rayTracedSunShadows, pathTracedReason);
    PipelineCheckbox(
        "Ray traced local light shadows",
        &rayTracing.localShadows,
        features.rayTracedLocalShadows,
        rayTracingReason != nullptr || features.restirPt ? pathTracedReason : "Local light shadows are off");
    PipelineCheckbox(
        "Ray traced reflections",
        &rayTracing.reflections,
        features.rayTracedReflections,
        rayTracingReason != nullptr || features.pathTracing ? pathTracedReason : "Reflections need TAA's history (TAA or DLSS)");
    PipelineCheckbox(
        "Ray traced ambient occlusion",
        &rayTracing.ambientOcclusion,
        features.rayTracedAmbientOcclusion,
        rayTracingReason != nullptr || features.pathTracing ? pathTracedReason : "Ambient occlusion is off");
    PipelineCheckbox(
        "DDGI probe occlusion",
        &rayTracing.probeOcclusion,
        features.probeOcclusion,
        rayTracingReason != nullptr || features.pathTracing ? pathTracedReason : "DDGI is off");
    if (features.probeOcclusion && ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Darkens the probes' light where spaces are smaller than a coarse level's spacing");
    }
    // The occlusion rays follow the two switches above.
    refresh();
    ImGui::BeginDisabled(!features.occlusionRays);
    DragIntInRange("Occlusion rays", &rayTracing.occlusionRays, 1, 8);
    ImGui::EndDisabled();
    PipelineCheckbox(
        "Denoise traced shadows",
        &rayTracing.denoise,
        features.rayTracedShadowDenoise,
        features.rayTracedSunShadows && rayTracing.sunShadows ? "DLSS ray reconstruction denoises them" : "No traced sun shadow");
    if (ImGui::SmallButton("Reset##rt"))
    {
        rayTracing = RayTracingSettings{};
        refresh();
    }
    OffNote(features.rayTracedEffects, rayTracingReason);
    ImGui::Unindent();

    // Path tracing in place of the ambient terms (also Render > Pipeline > Path Tracing): it needs the
    // hardware rays above, and says what it is doing below.
    ImGui::SeparatorText("Path tracing");
    PathTracingSettings& pathTracing = debug.pathTracing;
    ImGui::BeginDisabled(!features.pathTracingAvailable);
    if (ImGui::Checkbox("Enabled##pt", &pathTracing.enabled))
    {
        refresh();
    }
    ImGui::BeginDisabled(!features.pathTracing);
    DragIntInRange("Bounces##pt", &pathTracing.maxBounces, 1, 16);
    DragIntInRange("Light candidates##pt", &pathTracing.lightCandidates, 1, 32);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Local lights each path vertex resamples one from for its shadow ray");
    }
    // ReSTIR PT Enhanced in place of the plain path tracer: it carries the direct light too.
    if (ImGui::Checkbox("ReSTIR PT Enhanced##pt", &pathTracing.restir))
    {
        refresh();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Direct and indirect light in one reservoir, resampled across paired neighbours and frames\n"
                          "(Lin, Kettunen and Wyman 2026), instead of the lighting pass's lights and shadows.\n"
                          "Best with DLSS ray reconstruction.");
    }
    if (pathTracing.restir)
    {
        RestirPtSettings& restirPt = pathTracing.restirPt;
        ImGui::Indent();
        ImGui::Checkbox("Temporal reuse##restir", &restirPt.temporalReuse);
        ImGui::SameLine();
        ImGui::Checkbox("Spatial reuse##restir", &restirPt.spatialReuse);
        ImGui::Checkbox("Footprint reconnection##restir", &restirPt.footprintReconnection);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("The paper's dual ray footprint test; off, a fixed distance and two-vertex roughness");
        }
        if (restirPt.footprintReconnection)
        {
            DragFloatInRange("Footprint scale c##restir", &restirPt.footprintScale, 0.001f, 1.0f, "%.3f");
        }
        else
        {
            DragFloatInRange("Shortest reconnection (m)##restir", &restirPt.legacyDistance, 0.0f, 10.0f, "%.2f");
        }
        DragFloatInRange("Roughness threshold##restir", &restirPt.roughnessThreshold, 0.0f, 1.0f, "%.2f");
        DragFloatInRange("Confidence cap##restir", &restirPt.cap, 1.0f, 100.0f, "%.0f");
        ImGui::Checkbox("Decorrelation##restir", &restirPt.decorrelation);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Lowers the temporal confidence where neighbours share a sample (duplication map)");
        }
        ImGui::Checkbox("Colour noise reduction##restir", &restirPt.colorNoiseReduction);
        ImGui::Checkbox("Dual motion vectors##restir", &restirPt.dualMotionVectors);
        ImGui::Checkbox("Permutation sampling##restir", &restirPt.permutationSampling);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Temporal reuse takes a neighbour in the 2 x 2 quad: noise the denoiser can average");
        }
        ImGui::Checkbox("Russian roulette##restir", &restirPt.russianRoulette);
        ImGui::Checkbox("Accumulate (still camera)##restir", &restirPt.accumulate);
        static const char* kViews[] = {"Image", "Duplication map", "Reconnection vertex", "Confidence (log2)", "Path length", "Pairing check"};
        ImGui::Combo("View##restir", &restirPt.debugView, kViews, IM_ARRAYSIZE(kViews));
        if (ImGui::SmallButton("Reset##restir"))
        {
            restirPt = RestirPtSettings{};
        }
        ImGui::Unindent();
    }
    else
    {
        DragFloatInRange("Firefly clamp##pt", &pathTracing.fireflyClamp, 0.0f, 1000.0f, "%.1f");
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("The brightest a path vertex may add, in display units; 0 clamps nothing (reference)");
        }
        // DLSS ray reconstruction takes the raw paths and denoises them itself.
        const char* const rayReconstructionReason = "DLSS ray reconstruction denoises the paths";
        PipelineCheckbox("Accumulate##pt", &pathTracing.accumulate, features.pathTraceAccumulate, rayReconstructionReason);
        ImGui::BeginDisabled(!pathTracing.accumulate || !features.pathTraceAccumulate);
        DragIntInRange("Frames while moving##pt", &pathTracing.motionFrames, 1, 256);
        DragIntInRange("Frames while still##pt", &pathTracing.maxFrames, 1, 2048);
        ImGui::EndDisabled();
        PipelineCheckbox("Denoise##pt", &pathTracing.denoise, features.pathTraceDenoise, rayReconstructionReason);
    }
    if (ImGui::SmallButton("Reset##pt"))
    {
        pathTracing = PathTracingSettings{.enabled = pathTracing.enabled, .restir = pathTracing.restir};
        refresh();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (!state.pathTracingStatus.empty())
    {
        ImGui::TextDisabled("%s", state.pathTracingStatus.c_str());
    }

    ImGui::BeginDisabled(!features.sunShadowMap);
    DragFloatInRange("Shadow distance (m)", &debug.shadowDistance, 10.0f, 5000.0f, "%.0f");
    ImGui::EndDisabled();
    // The forward-only order has no motion vectors, so TAA is off there whatever this says; DLSS
    // takes its place while it resolves.
    if (PipelineCheckbox(
            "Temporal anti-aliasing",
            &debug.taa,
            features.taa,
            state.DlssResolves() ? "DLSS resolves in its place" : "Off in the forward-only order"))
    {
        refresh();
    }
    PipelineCheckbox("Specular anti-aliasing", &debug.specularAntiAliasing, features.specularAntiAliasing, khronosReason);
    // Bloom needs no motion vectors, so unlike what follows it works in the forward-only order.
    ImGui::SeparatorText("Glare (bloom)");
    ImGui::BeginDisabled(!features.bloom);
    ImGui::Checkbox("Enabled##bloom", &debug.bloom.enabled);
    ImGui::BeginDisabled(!debug.bloom.enabled);
    // 1 is the diffraction the exposure's aperture produces; the Camera panel shows that aperture.
    DragFloatInRange("Strength##bloom", &debug.bloom.strength, 0.0f, 4.0f, "%.2f");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    OffNote(features.bloom, khronosReason);

    // The trace needs TAA's history for its colour and the G-buffer, so it is off without TAA
    // and in the forward-only order; path tracing traces its own reflections.
    ImGui::SeparatorText("Screen-space reflections");
    ImGui::BeginDisabled(!features.ssr);
    ImGui::Checkbox("Enabled##ssr", &debug.ssr.enabled);
    ImGui::BeginDisabled(!debug.ssr.enabled);
    DragFloatInRange("Max roughness##ssr", &debug.ssr.maxRoughness, 0.05f, 1.0f, "%.2f");
    DragFloatInRange("Max distance (m)##ssr", &debug.ssr.maxDistance, 1.0f, 200.0f, "%.0f");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    const char* const ambientReason = AmbientOffReason(debug, features);
    OffNote(features.ssr, ambientReason != nullptr ? ambientReason : "Needs TAA's history: turn on TAA or DLSS");

    // MINIENGINE_toon materials (anime characters) over the rest of the scene.
    ImGui::SeparatorText("Anime characters");
    DragFloatInRange("Exposure (EV)##toon", &debug.toonExposureEv, -4.0f, 4.0f, "%+.2f");

    ImGui::SeparatorText("Output");
    // HDR10 when the display offers it (GT7's HDR curve); the UI keeps its SDR brightness.
    ImGui::Checkbox("HDR output", &debug.hdrOutput);
    ImGui::BeginDisabled(!debug.hdrOutput);
    DragFloatInRange("Display peak (nits)", &debug.hdrPeakNits, 250.0f, 10000.0f, "%.0f");
    ImGui::EndDisabled();
    // Also Render > Tone Mapping. The Khronos reference view always uses PBR Neutral.
    static constexpr std::array<const char*, 3> kToneMapperNames = {"GT7", "PBR Neutral", "None (clipped)"};
    int toneMapper = static_cast<int>(debug.toneMapper);
    ImGui::BeginDisabled(!features.toneMapperChoice);
    if (ImGui::Combo("Tone mapping", &toneMapper, kToneMapperNames.data(), static_cast<int>(kToneMapperNames.size())))
    {
        debug.toneMapper = static_cast<ToneMapper>(toneMapper);
    }
    ImGui::EndDisabled();
    // The forward-only order never writes the G-buffer, so there is nothing to view.
    ImGui::BeginDisabled(!features.gbufferViews);
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
    ImGui::EndDisabled();

    // The forward-only order and the Khronos reference view run no AO pass, and path tracing's light
    // replaces it.
    ImGui::SeparatorText("Ambient occlusion");
    AoSettings& ao = debug.ao;
    ImGui::BeginDisabled(!features.ao);
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
    OffNote(features.ao, ambientReason);

    // One bounce of the lit image onto its neighbours, through the same bitmask as the AO.
    ImGui::SeparatorText("Screen-space GI");
    GiSettings& gi = debug.gi;
    ImGui::BeginDisabled(!features.gi);
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
    OffNote(features.gi, ambientReason);

    // Probes around the camera that trace the scene: the diffuse ambient's light, off screen and
    // bounced any number of times. Forward-shaded surfaces use them too, so the forward-only
    // order and path tracing (for the forward-shaded surfaces) keep them.
    ImGui::SeparatorText("DDGI");
    DdgiSettings& ddgi = debug.ddgi;
    ImGui::BeginDisabled(!features.ddgi);
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
    ImGui::EndDisabled();
    OffNote(features.ddgi, khronosReason);
}
}
