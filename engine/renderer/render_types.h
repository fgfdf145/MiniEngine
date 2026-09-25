#pragma once

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
    Sheen = 12
};

// Visibility bitmask ambient occlusion. Not persisted. The pass clamps every value again before the
// shader sees it, so the editor's slider ranges are a convenience, not a guarantee.
struct AoSettings
{
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

// Renderer switches the editor owns and the backend reads when it builds each frame. Plain data,
// handed over by copy in EditorUiFrameResult.
// Glare (bloom): each pixel gives the share of its energy that diffraction through the exposure's
// aperture carries past 2 pixels to a blur of its surroundings (see glare.h). Not persisted, like the
// rest of RenderDebugSettings.
struct BloomSettings
{
    bool enabled = true;
    // Multiplies the diffraction energy; 1 is the physical value.
    float strength = 1.0f;
};

// Screen-space reflections (see ssr_common.glsl): replace the environment's specular radiance where
// the screen shows what a glossy surface reflects. Not persisted, like the rest of RenderDebugSettings.
struct SsrSettings
{
    bool enabled = true;
    // Rougher lobes are left to the prefiltered environment; the trace fades out from 0.4 to this.
    // 0.8 keeps rough stone (Sponza's floor is 0.6-0.8) in the trace: a blurry, dim reflection that
    // mostly shows as local occlusion of the environment, which the filters and TAA keep quiet.
    float maxRoughness = 0.8f;
    // How far a ray is marched, in metres.
    float maxDistance = 30.0f;
};

struct RenderDebugSettings
{
    GBufferDebugView gbufferView = GBufferDebugView::Off;
    // Records the forward-only order instead of the deferred one: the comparison switch that makes
    // pixel equivalence something a reviewer flips rather than judges.
    bool forwardOnly = false;
    // Looks local lights up through the light cluster grid. Off, every pixel loops over all of them:
    // the comparison path, which must render the same image.
    bool clusteredLighting = true;
    // Shadow maps for point, spot and area lights (the local shadow atlas). Off, no local light
    // casts a shadow, as before they existed.
    bool localLightShadows = true;
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
};
}
