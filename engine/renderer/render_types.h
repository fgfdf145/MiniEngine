#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace me
{

// The GPU's device-local memory as the render thread last measured it (docs/design/2026-10-07-vram-budget-design.md),
// in bytes. World streaming fits the scene's content in Headroom.
struct GpuMemoryReport
{
    // The frame it was measured on; 0 before the first measurement.
    uint64_t serial = 0;
    // The driver's budget and this process's usage of the device-local heaps (VK_EXT_memory_budget).
    uint64_t budget = 0;
    uint64_t usage = 0;
    // The scene's buffers, textures and acceleration structures: the memory pool's allocations.
    uint64_t worldBytes = 0;
    // What the render targets would need on top of today's to fill the display, plus a margin, so
    // that going fullscreen needs no room the world holds.
    uint64_t reserve = 0;

    // The room left for more of the world; negative when over budget.
    int64_t Headroom() const
    {
        return static_cast<int64_t>(budget) - static_cast<int64_t>(usage) - static_cast<int64_t>(reserve);
    }
};

struct RenderExtent
{
    uint32_t width = 0;
    uint32_t height = 0;

    bool IsValid() const
    {
        return width > 0 && height > 0;
    }
};

// The scene's render size for a viewport panel of widthPoints x heightPoints (the UI's unit), on a
// display with pixelsPerPoint (2 on Retina), at renderScale: the display's pixels, scaled, rounded, at
// least 1 x 1. Rendering at the size in points showed a quarter of the pixels on a Retina display.
inline RenderExtent ScaleViewportExtent(float widthPoints, float heightPoints, float pixelsPerPoint, float renderScale)
{
    const float scale = pixelsPerPoint * renderScale;
    const auto pixels = [scale](float points)
    {
        const float value = std::round(points * scale);
        return value >= 1.0f ? static_cast<uint32_t>(value) : 1u;
    };
    return RenderExtent{pixels(widthPoints), pixels(heightPoints)};
}

// What the viewport shows instead of the tone mapped image. The numeric values are the
// tonemap.frag push constant and must match the GBUFFER_VIEW_* constants there.
enum class GBufferDebugView : uint32_t
{
    Off = 0,
    Albedo = 1,
    Normal = 2,
    GeometricNormal = 3,
    Surface = 4,
    Emissive = 5,
    MotionVectors = 6,
    AmbientOcclusion = 7,
    // How many local lights each pixel's cluster lists, as a heat map. Drawn by the lighting pass,
    // so Blend surfaces still show shaded on top of it.
    LightClusters = 8,
    // GB5 as stored: rgb sqrt(dielectric F0), black for pixels with the default specular.
    Specular = 9,
    // The resolved screen-space reflections, tone mapped, dimmed where their confidence is low.
    Reflections = 10,
    // GB6 as stored: r coat factor, g coat roughness, b anisotropy angle.
    Coat = 11,
    // GB7 as stored: the sheen's colour.
    Sheen = 12,
    // The resolved one-bounce indirect diffuse light (SceneGi), tone mapped, before the albedo.
    IndirectDiffuse = 13,
    // One primary ray per pixel through the DDGI ray scene: the ray materials' albedo lit by the
    // first directional light through traced shadow rays (ddgi_debug.comp). Red: a single-sided back
    // face.
    RayTraced = 14,
    // What the DDGI probes send each pixel's surface (irradiance / pi, pre-exposed), tone mapped, before
    // the albedo; black outside the volume.
    DdgiIrradiance = 15,
    // The image with every DDGI probe drawn as a small sphere of its irradiance: red inactive, blue not
    // yet updated for where it is.
    DdgiProbes = 16,
    // The ray traced sun shadow as the lighting reads it (white lit, black shadowed), filtered unless
    // RayTracingSettings::denoise is off. White where it does not run.
    RayTracedShadow = 17,
    // The ray traced DDGI probe occlusion (SceneAo's g): the share of the probes' light the lighting
    // keeps. White where it does not run.
    ProbeOcclusion = 18
};

// The last view, for the readers that clamp a stored number.
inline constexpr GBufferDebugView kLastGBufferDebugView = GBufferDebugView::ProbeOcclusion;

// Visibility bitmask ambient occlusion. The pass clamps every value again before the
// shader sees it, so the editor's slider ranges are a convenience, not a guarantee.
struct AoSettings
{
    bool operator==(const AoSettings&) const = default;

    bool enabled = true;
    // World-space search radius, in metres.
    float radius = 1.5f;
    // Assumed thickness of every depth sample, in metres: what makes this a visibility bitmask
    // rather than a horizon, so thin geometry does not shadow everything behind it.
    float thickness = 0.25f;
    int sliceCount = 2;
    // Per side of each slice.
    int stepCount = 8;
    bool spatialFilter = true;
    bool temporalFilter = true;
};

// One bounce of diffuse light in screen space (see gi_trace.comp): the lit image, as far as the
// screen shows it, lights its neighbours through the visibility bitmask the AO marches.
// The pass clamps every value again before the shader sees it.
struct GiSettings
{
    bool operator==(const GiSettings&) const = default;

    bool enabled = true;
    // World-space search radius, in metres: how far light bounces.
    float radius = 3.0f;
    // Assumed thickness of every depth sample, in metres, as for the AO.
    float thickness = 0.5f;
    int sliceCount = 2;
    // Per side of each slice.
    int stepCount = 12;
    // Multiplies the bounced light; 1 is the physical value.
    float strength = 1.0f;
    bool spatialFilter = true;
    bool temporalFilter = true;
};

// Renderer switches the editor owns and the backend reads when it builds each frame. Plain data,
// handed over by copy in EditorUiFrameResult.
// Glare (bloom): each pixel gives the share of its energy that diffraction through the exposure's
// aperture carries past 2 pixels to a blur of its surroundings (see glare.h).
struct BloomSettings
{
    bool operator==(const BloomSettings&) const = default;

    bool enabled = true;
    // Multiplies the diffraction energy; 1 is the physical value.
    float strength = 1.0f;
};

// Screen-space reflections (see ssr_common.glsl): replace the environment's specular radiance where
// the screen shows what a glossy surface reflects.
struct SsrSettings
{
    bool operator==(const SsrSettings&) const = default;

    bool enabled = true;
    // Rougher lobes are left to the prefiltered environment; the trace fades out from 0.4 to this.
    // 0.8 keeps rough stone (Sponza's floor is 0.6-0.8) in the trace: a blurry, dim reflection that
    // mostly shows as local occlusion of the environment, which the filters and TAA keep quiet.
    float maxRoughness = 0.8f;
    // How far a ray is marched, in metres.
    float maxDistance = 30.0f;
};

// Cascaded DDGI (docs/design/2026-09-27-ddgi-design.md): probes around the camera that trace the
// ray scene every frame and light every surface's diffuse ambient with what they see.
// The renderer clamps every value again before the GPU sees it.
struct DdgiSettings
{
    bool operator==(const DdgiSettings&) const = default;

    bool enabled = true;
    // Levels of kDdgiGridSize probes, the spacing doubling per level.
    int levels = 4;
    // Level 0's probe spacing, metres.
    float baseSpacing = 1.0f;
    int probesPerFrame = 2048;
    // The share of a probe's previous value each update keeps.
    float hysteresis = 0.97f;
    // How far a lookup moves off the surface, along the normal and toward the viewer, in spacings.
    float normalBias = 0.1f;
    float viewBias = 0.2f;
    // The level the probe debug view draws.
    int probeViewLevel = 0;
};

// Effects traced through the ray scene with the GPU's ray queries (docs/design/
// 2026-10-07-ray-traced-effects-design.md). Each replaces its screen-space or shadow-map
// counterpart while hardware ray tracing runs (the device has ray queries and
// RenderDebugSettings::hardwareRayTracing is on); without it the counterpart stays.
struct RayTracingSettings
{
    bool operator==(const RayTracingSettings&) const = default;

    // The sun's shadow: one ray a pixel toward a point on the sun's disk, filtered, instead of the
    // cascades. Soft as the sun's size makes it, sharp at contact, and as far as the scene reaches.
    bool sunShadows = true;
    // Ambient occlusion traced within AoSettings::radius instead of the screen-space bitmask: it sees
    // what is off screen or hidden behind the surface.
    bool ambientOcclusion = true;
    // The DDGI probes' light scaled by the share of short rays that escape where a coarse level
    // answers, so spaces smaller than its spacing are not lit as if open (the DDGI design's
    // "Occlusion Finer Than the Probes").
    bool probeOcclusion = true;
    // Rays a pixel for the two above, at half resolution.
    int occlusionRays = 2;
    // Reflections traced through the scene instead of marched against the depth buffer: what lies off
    // screen or behind something is reflected too, shaded at the hit.
    bool reflections = true;
    // Point, spot and area lights' shadows traced from the lighting pass instead of the shadow atlas.
    bool localShadows = true;
    // The traced shadow's spatial and temporal filters. Off, the raw one-ray result.
    bool denoise = true;
};

// ReSTIR PT Enhanced (Lin, Kettunen and Wyman 2026; docs/design/2026-10-07-restir-pt-enhanced-design.md),
// the path tracer's resampled mode: every opaque deferred pixel's reflected light, direct and indirect
// in one reservoir, resampled across neighbours and frames. Its own switches; the bounces and light
// candidates are PathTracingSettings'.
struct RestirPtSettings
{
    bool operator==(const RestirPtSettings&) const = default;

    // Resampling with last frame's reservoir and with three paired neighbours (the paper's section 3).
    bool temporalReuse = true;
    bool spatialReuse = true;
    // The dual ray footprint test and single-vertex roughness test (section 4); off, Lin et al. 2022's
    // distance and two-vertex roughness thresholds.
    bool footprintReconnection = true;
    // c in the paper's equation 5.
    float footprintScale = 0.02f;
    // The least roughness a vertex may reconnect from.
    float roughnessThreshold = 0.2f;
    // The old criterion's shortest reconnection, in metres.
    float legacyDistance = 0.1f;
    // The temporal confidence cap, lowered where neighbours share samples (section 5's duplication map):
    // cap = lerp(cap, capMin, duplication ^ capGamma) while decorrelation is on. The paper's 20 keeps a
    // sample on a pixel for so long that DLSS ray reconstruction takes its slowly changing noise for
    // texture (blotchy car paint); 4 measured closer to the reference with it.
    bool decorrelation = true;
    float cap = 4.0f;
    float capMin = 1.0f;
    float capGamma = 0.1f;
    // Shading with the spatial reuse's vector-valued weights (section 6.3).
    bool colorNoiseReduction = true;
    // Disoccluded pixels look for a temporal neighbour along the occluder's motion (section 6.4).
    bool dualMotionVectors = true;
    // Temporal reuse takes its history from a pixel of the 2 x 2 quad chosen per frame (RTXDI's
    // permutation sampling), so a pixel does not keep resampling its own history and the noise the
    // denoiser sees moves.
    bool permutationSampling = true;
    // Russian roulette on the initial paths only (section 6.2.4).
    bool russianRoulette = true;
    // Averages the frames while the camera and scene stand still: with both reuses off, an unbiased
    // reference to compare against.
    bool accumulate = false;
    // 0 the image; 1 the duplication map, 2 the reconnection vertex's index, 3 the confidence (log2),
    // 4 the path length, 5 green where the pairing textures' links are mutual (restir_pt_common.glsl's
    // PT_DEBUG_*).
    int debugView = 0;
};

// GPU path tracing (docs/design/2026-10-07-path-tracing-design.md): every opaque deferred pixel's
// indirect light, path traced from its G-buffer surface through the ray scene with hardware ray
// queries, in place of the ambient terms (DDGI, the sky's split sum, reflections, AO and the
// screen-space GI). The lighting pass keeps the direct lights and their ray traced shadows. While the
// camera and the scene stand still the frames accumulate toward a reference; DLSS ray reconstruction,
// when it runs, denoises the raw paths instead of the engine's filters. The Render > Pipeline > Path
// Tracing mode; it needs hardware ray tracing and falls back to the hybrid image without it.
// With restir on, ReSTIR PT Enhanced (RestirPtSettings) runs instead and carries the direct light too.
struct PathTracingSettings
{
    bool operator==(const PathTracingSettings&) const = default;

    bool enabled = false;
    // Surfaces a path visits after the G-buffer's: 1 is one bounce of indirect light (0 under ReSTIR PT
    // is direct light only).
    int maxBounces = 3;
    // The most a path's vertex may add, in HDR target units (pre-exposed luminance): bright, rarely
    // found light (a small lamp a diffuse bounce happens to hit) otherwise shows as speckles for many
    // frames. 0 adds everything, as a reference should.
    float fireflyClamp = 32.0f;
    // The local lights next event estimation picks one from, by resampling, at each path vertex
    // (ReSTIR PT: at the first surface; deeper vertices take this / bounce^2, at least one).
    int lightCandidates = 8;
    // The temporal accumulation: frames reprojected through the motion vectors and averaged, up to
    // motionFrames while anything moves and up to maxFrames while everything stands still.
    bool accumulate = true;
    int motionFrames = 32;
    int maxFrames = 2048;
    // The edge-aware spatial filter after it, which fades out as a still image converges.
    bool denoise = true;
    // ReSTIR PT Enhanced instead of the plain path tracer: direct and indirect light resampled across
    // neighbours and frames (best with DLSS ray reconstruction, as the paper is evaluated).
    bool restir = false;
    RestirPtSettings restirPt;
};

// The operator the tone mapping pass applies to the shaded image (the G-buffer views pick their own).
// The numeric values are not the tonemap.frag push constant: VulkanTonemapPass maps them.
enum class ToneMapper : uint32_t
{
    // GT7's operator, SDR or HDR10 (gt7_tonemap.glsl).
    Gt7 = 0,
    // Khronos PBR Neutral on the exposed value, without the Khronos reference view's viewer encoding.
    PbrNeutral = 1,
    // The exposed value clipped to the display's range: no curve, to see what the operator changes.
    None = 2
};

// NVIDIA DLSS (VulkanDlss): off, DLAA at the output size, or super resolution from DLSS's own render
// size for each quality. Where DLSS is unavailable every mode falls back to the engine's TAA at the
// render scale. The values are what the settings files store.
enum class DlssMode : uint32_t
{
    Off = 0,
    Dlaa = 1,
    Quality = 2,
    Balanced = 3,
    Performance = 4,
    UltraPerformance = 5
};

// The DLSS model (NGX's render preset) every mode runs. Default leaves the choice to DLSS: K for DLAA,
// Quality and Balanced, M for Performance and L for Ultra Performance in SDK 310.9. J and K are the
// first transformer models, L and M the second. The values are what the settings files store, not
// NGX's numbers.
enum class DlssPreset : uint32_t
{
    Default = 0,
    J = 1,
    K = 2,
    L = 3,
    M = 4
};

// Saved in miniengine.settings.json (EngineViewSettings), all but the G-buffer view: a debug view
// left on should not survive a restart.
struct RenderDebugSettings
{
    bool operator==(const RenderDebugSettings&) const = default;

    GBufferDebugView gbufferView = GBufferDebugView::Off;
    // The Khronos reference view uses PBR Neutral whatever this says.
    ToneMapper toneMapper = ToneMapper::Gt7;
    // Records the forward-only order instead of the deferred one: the comparison switch that makes
    // pixel equivalence something a reviewer flips rather than judges.
    bool forwardOnly = false;
    // Looks local lights up through the light cluster grid. Off, every pixel loops over all of them:
    // the comparison path, which must render the same image.
    bool clusteredLighting = true;
    // Shadow maps for point, spot and area lights (the local shadow atlas). Off, no local light
    // casts a shadow, as before they existed.
    bool localLightShadows = true;
    // Rays through the ray scene (DDGI's probes and its debug views) use the GPU's ray queries against
    // acceleration structures when the device has them. Off, or without them, they walk the ray
    // scene's own hierarchies in compute: the comparison path, which must find the same hits.
    bool hardwareRayTracing = true;
    // The effects traced with hardware ray tracing; each falls back to its screen-space or shadow-map
    // counterpart without it.
    RayTracingSettings rayTracing;
    // Path tracing in place of the ambient terms (or, as ReSTIR PT, of all the lighting), where hardware
    // rays run.
    PathTracingSettings pathTracing;
    // How far from the camera the sun's cascaded shadows reach, in metres (ShadowCascadeSettings::
    // maxDistance). The same four cascades cover it, so a longer reach gives coarser shadows.
    float shadowDistance = 80.0f;
    // Temporal anti-aliasing: jittered projection plus the TAA resolve. Off, the frame is neither
    // jittered nor resolved, and renders exactly as it did before TAA existed.
    bool taa = true;
    // Geometric specular anti-aliasing: widens specular lobes by how much the normal varies across
    // each pixel. Off, roughness reaches the lighting exactly as the material gives it.
    bool specularAntiAliasing = true;
    BloomSettings bloom;
    SsrSettings ssr;
    // Presents to an HDR10 swapchain when the display offers one (see hdr_output.glsl), tone mapped
    // with GT7's HDR curve for this peak luminance in cd/m^2, which Vulkan cannot query.
    bool hdrOutput = false;
    float hdrPeakNits = 1000.0f;
    // The anime characters' brightness over the physical scene's, in EV (toon_pass.h). Their shading
    // is display-referred in AnimateApp, where the lit albedo reaches the screen as it is; at 0 their
    // lit side is as bright as a white diffuse surface facing the same light, which keeps them in step
    // with the scene round them (a stop up was brighter than anything white beside them).
    float toonExposureEv = 0.0f;
    AoSettings ao;
    GiSettings gi;
    DdgiSettings ddgi;
    // The Khronos reference view: renders as the Khronos glTF Sample Viewer does by default, to
    // compare against it (docs/design/2026-09-26-khronos-reference-comparison-design.md).
    // PBR Neutral tone mapping, an HDRI texel of 1 exposed to 1, no auto white balance, no
    // glare/bloom, AO, GI or SSR, and the viewer's camera framing. Off, nothing changes.
    bool khronosReference = false;
    // The scene's resolution as a share of the viewport's pixels (see ScaleViewportExtent), from 0.25
    // to 1: below 1 it renders fewer pixels and is stretched to fill the panel. DLSS, when it runs,
    // picks the render size itself and outputs every pixel of the viewport.
    float renderScale = 1.0f;
    // Replaces TAA, in the deferred order, where the device runs DLSS.
    DlssMode dlssMode = DlssMode::Off;
    DlssPreset dlssPreset = DlssPreset::Default;
    // While DLSS runs, its ray reconstruction (DLSS-D) where the device has it: one network denoises the
    // ray traced effects and upscales, so the engine's own traced shadow filter steps aside.
    bool dlssRayReconstruction = true;
};
}
