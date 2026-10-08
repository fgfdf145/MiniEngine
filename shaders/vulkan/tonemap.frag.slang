#include "shader_helpers.slang"
#include "gt7_tonemap.slang"
#include "pre_exposure.slang"
#include "hdr_output.slang"
#include "pbr_neutral.slang"
#include "gbuffer_common.slang"
#include "gbuffer_inputs.slang"

[[vk::binding(0, 0)]] Sampler2D hdrTexture;

// Must match GBufferDebugView in engine/renderer/render_types.h.
static const uint GBUFFER_VIEW_OFF = 0u;
static const uint GBUFFER_VIEW_ALBEDO = 1u;
static const uint GBUFFER_VIEW_NORMAL = 2u;
static const uint GBUFFER_VIEW_GEOMETRIC_NORMAL = 3u;
static const uint GBUFFER_VIEW_SURFACE = 4u;
static const uint GBUFFER_VIEW_EMISSIVE = 5u;
static const uint GBUFFER_VIEW_MOTION_VECTORS = 6u;
static const uint GBUFFER_VIEW_AMBIENT_OCCLUSION = 7u;
static const uint GBUFFER_VIEW_LIGHT_CLUSTERS = 8u;
static const uint GBUFFER_VIEW_SPECULAR = 9u;
static const uint GBUFFER_VIEW_REFLECTIONS = 10u;
static const uint GBUFFER_VIEW_COAT = 11u;
static const uint GBUFFER_VIEW_SHEEN = 12u;
static const uint GBUFFER_VIEW_INDIRECT_DIFFUSE = 13u;
static const uint GBUFFER_VIEW_RAY_TRACED = 14u;
static const uint GBUFFER_VIEW_DDGI_IRRADIANCE = 15u;
static const uint GBUFFER_VIEW_DDGI_PROBES = 16u;
static const uint GBUFFER_VIEW_RAY_TRACED_SHADOW = 17u;
static const uint GBUFFER_VIEW_PROBE_OCCLUSION = 18u;

// Must match kOperator* in engine/renderer/vulkan/tonemap_pass.cpp.
static const uint TONEMAP_OPERATOR_GT7 = 0u;
static const uint TONEMAP_OPERATOR_KHRONOS_REFERENCE = 1u;
static const uint TONEMAP_OPERATOR_PBR_NEUTRAL = 2u;
static const uint TONEMAP_OPERATOR_NONE = 3u;

// Must match TonemapPushConstants in engine/renderer/vulkan/tonemap_pass.cpp.
struct TonemapConstants
{
    uint gbufferView;
    // 1 for HDR10 output: GT7's HDR curve for peakNits, written relative to kUiWhiteNits.
    uint hdrOutput;
    float peakNits;
    // One of the TONEMAP_OPERATOR_* values below.
    uint toneOperator;
    // Auto white balance, linear Rec.709 to linear Rec.709, as three columns (xyz used).
    float4 whiteBalance[3];
};
[[vk::push_constant]] ConstantBuffer<TonemapConstants> constants;

struct FragmentInput
{
    [[vk::location(0)]] float2 fragTexCoord;
};

struct FragmentOutput
{
    [[vk::location(0)]] float4 outColor : SV_Target0;
};

[shader("fragment")]
FragmentOutput main(FragmentInput input)
{
    FragmentOutput output;
    float3 color;

    if (constants.gbufferView == GBUFFER_VIEW_ALBEDO)
    {
        // Sampled through the _SRGB format, so already linear; the sRGB LDR target re-encodes it.
        // Shows albedo as stored: unshaded, unexposed and not tone mapped.
        color = gbufferAlbedo.Sample(input.fragTexCoord).rgb;
    }
    else if (constants.gbufferView == GBUFFER_VIEW_NORMAL)
    {
        // World-space shading normal mapped to [0, 1]. Unwritten pixels decode to +Z and read as
        // blue.
        color = DecodeNormalOctahedral(gbufferNormal.Sample(input.fragTexCoord).rg) * 0.5 + 0.5;
    }
    else if (constants.gbufferView == GBUFFER_VIEW_GEOMETRIC_NORMAL)
    {
        // The interpolated, face-flipped vertex normal the shadow lookup offsets along.
        color = DecodeNormalOctahedral(gbufferNormal.Sample(input.fragTexCoord).ba) * 0.5 + 0.5;
    }
    else if (constants.gbufferView == GBUFFER_VIEW_SURFACE)
    {
        // r = metallic, g = roughness, b = occlusion.
        color = gbufferSurface.Sample(input.fragTexCoord).rgb;
    }
    else if (constants.gbufferView == GBUFFER_VIEW_EMISSIVE)
    {
        // Emissive is pre-exposed radiance, so it is tone mapped exactly as the shaded image is.
        float3 emissive = min(gbufferEmissive.Sample(input.fragTexCoord).rgb, float3(65504.0));
        color = TonemapFrameBufferRec709(emissive);
    }
    else if (constants.gbufferView == GBUFFER_VIEW_MOTION_VECTORS)
    {
        // Mid-grey is still. Red grows with rightward motion and green with downward motion,
        // saturating at 4 pixels per frame: the editor runs at hundreds of frames per second, where
        // walking the camera moves a surface only a pixel or two per frame. Background pixels hold
        // no vector and read grey.
        float2 pixels = gbufferVelocity.Sample(input.fragTexCoord).rg * float2(TextureSize(gbufferVelocity, 0));
        color = float3(clamp(0.5 + pixels / 8.0, 0.0, 1.0), 0.5);
    }
    else if (constants.gbufferView == GBUFFER_VIEW_AMBIENT_OCCLUSION)
    {
        // The resolved screen-space AO alone, white where nothing occludes.
        color = float3(sceneAo.Sample(input.fragTexCoord).r);
    }
    else if (constants.gbufferView == GBUFFER_VIEW_RAY_TRACED_SHADOW)
    {
        // The traced sun visibility the lighting reads (rt_shadow_filter.comp); its g says whether it
        // ran this frame, so the view is white where it did not.
        float4 shadow = sceneShadow.Sample(input.fragTexCoord);
        color = float3(shadow.g > 0.5 ? shadow.r : 1.0);
    }
    else if (constants.gbufferView == GBUFFER_VIEW_PROBE_OCCLUSION)
    {
        // The ray traced DDGI probe occlusion beside the AO in SceneAo.
        color = float3(sceneAo.Sample(input.fragTexCoord).g);
    }
    else if (constants.gbufferView == GBUFFER_VIEW_SPECULAR)
    {
        // GB5 as stored: sqrt of the dielectric F0. Pixels with the default specular are black.
        color = gbufferSpecular.Sample(input.fragTexCoord).rgb;
    }
    else if (constants.gbufferView == GBUFFER_VIEW_COAT)
    {
        // GB6 as stored: red the coat's factor, green its roughness, blue the anisotropy's angle.
        color = gbufferCoat.Sample(input.fragTexCoord).rgb;
    }
    else if (constants.gbufferView == GBUFFER_VIEW_SHEEN)
    {
        // GB7 as stored: the sheen's colour (its roughness, in alpha, does not show).
        color = gbufferSheen.Sample(input.fragTexCoord).rgb;
    }
    else if (constants.gbufferView == GBUFFER_VIEW_REFLECTIONS)
    {
        // What the screen-space trace found, in HDR target units, tone mapped like the image and
        // scaled by its confidence: black where the environment is used instead.
        float4 reflection = sceneReflections.Sample(input.fragTexCoord);
        color = TonemapFrameBufferRec709(min(reflection.rgb, float3(65504.0))) * reflection.a;
    }
    else if (constants.gbufferView == GBUFFER_VIEW_INDIRECT_DIFFUSE || constants.gbufferView == GBUFFER_VIEW_DDGI_IRRADIANCE ||
             constants.gbufferView == GBUFFER_VIEW_DDGI_PROBES)
    {
        // The light arriving by one bounce, or from the DDGI probes (ddgi_debug.comp wrote it over
        // SceneGi), in HDR target units, before any surface's albedo, tone mapped like the image.
        color = TonemapFrameBufferRec709(min(sceneGi.Sample(input.fragTexCoord).rgb, float3(65504.0)));
    }
    else if (constants.gbufferView == GBUFFER_VIEW_RAY_TRACED)
    {
        // ddgi_debug.comp wrote it over SceneGi: albedo-like values, shown as they are.
        color = sceneGi.Sample(input.fragTexCoord).rgb;
    }
    else if (constants.gbufferView == GBUFFER_VIEW_LIGHT_CLUSTERS)
    {
        // The lighting pass wrote heat colours times kFrameBufferUnitsPerExposed; scaling back shows
        // them as they were meant, without the operator bending their hues. Blend surfaces, shaded
        // on top by the forward pass, are exposed but not tone mapped here.
        color = min(hdrTexture.Sample(input.fragTexCoord).rgb * kExposedPerFrameBufferUnit, float3(1.0));
    }
    else
    {
        // The HDR target already holds pre-exposed values in the operator's unit (see
        // pre_exposure.slang). Clamp below fp16's maximum before the operator: an infinite input
        // would turn into NaN inside it and show a very bright pixel as black.
        color = min(hdrTexture.Sample(input.fragTexCoord).rgb, float3(65504.0));

        // Auto white balance, before the operator: a partial chromatic adaptation toward D65 (see
        // engine/renderer/white_balance.h). It can push a saturated colour slightly negative; the
        // operator clamps at zero.
        color = mul(MatrixFromColumns(constants.whiteBalance[0].xyz, constants.whiteBalance[1].xyz, constants.whiteBalance[2].xyz), color);

        if (constants.toneOperator == TONEMAP_OPERATOR_KHRONOS_REFERENCE || constants.toneOperator == TONEMAP_OPERATOR_PBR_NEUTRAL)
        {
            // The Sample Viewer's operator on the exposed value (its exposure 1.0 is the engine's
            // exposed 1.0). Display linear; with HDR10 output it is shown relative to the UI white,
            // as SDR content is.
            color = KhronosPbrNeutral(max(color * kExposedPerFrameBufferUnit, float3(0.0f)));
            // The Khronos reference view on an SDR display also takes the viewer's encoding: a 2.2
            // gamma instead of the sRGB curve.
            if (constants.toneOperator == TONEMAP_OPERATOR_KHRONOS_REFERENCE && constants.hdrOutput == 0u)
            {
                color = KhronosViewerOutputForSrgbTarget(color);
            }
        }
        else if (constants.toneOperator == TONEMAP_OPERATOR_NONE)
        {
            // No curve: the exposed value as display linear, clipped where the display ends. With
            // HDR10 output, frame-buffer units become nits relative to the UI white, up to the peak.
            if (constants.hdrOutput != 0u)
            {
                color = clamp(color * (Gt7FrameBufferToPhysical(1.0f) / kUiWhiteNits), float3(0.0f), float3(constants.peakNits / kUiWhiteNits));
            }
            else
            {
                color = clamp(color * kExposedPerFrameBufferUnit, float3(0.0f), float3(1.0f));
            }
        }
        // GT7's operator (see gt7_tonemap.slang). The result is display-referred linear Rec.709;
        // the LDR target's sRGB format applies the transfer function on write.
        else if (constants.hdrOutput != 0u)
        {
            // Frame-buffer units (1.0 = 100 cd/m^2) up to the display peak, then relative to the UI
            // white that imgui_hdr10.frag maps to kUiWhiteNits.
            color = TonemapFrameBufferRec709Hdr(color, constants.peakNits) * (Gt7FrameBufferToPhysical(1.0f) / kUiWhiteNits);
        }
        else
        {
            color = TonemapFrameBufferRec709(color);
        }
    }

    // This pass is the sole writer of the LDR target and knows coverage is total, so it writes
    // alpha explicitly rather than relying on the RGB-only color write mask the material
    // pipelines use to keep the attachment's clear alpha intact. ImGui composites the viewport
    // image over the editor, so this alpha must be 1.0.
    output.outColor = float4(color, 1.0);
    return output;
}
