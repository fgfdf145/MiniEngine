#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace me
{

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
    DdgiProbes = 16
};

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
};
}
