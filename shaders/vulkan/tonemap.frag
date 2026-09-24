#version 450
#extension GL_GOOGLE_include_directive : require

#include "gt7_tonemap.glsl"
#include "pre_exposure.glsl"
#include "hdr_output.glsl"
#include "gbuffer_common.glsl"
#include "gbuffer_inputs.glsl"

layout(set = 0, binding = 0) uniform sampler2D hdrTexture;

// Must match GBufferDebugView in engine/renderer/render_types.h.
const uint GBUFFER_VIEW_OFF = 0u;
const uint GBUFFER_VIEW_ALBEDO = 1u;
const uint GBUFFER_VIEW_NORMAL = 2u;
const uint GBUFFER_VIEW_GEOMETRIC_NORMAL = 3u;
const uint GBUFFER_VIEW_SURFACE = 4u;
const uint GBUFFER_VIEW_EMISSIVE = 5u;
const uint GBUFFER_VIEW_MOTION_VECTORS = 6u;
const uint GBUFFER_VIEW_AMBIENT_OCCLUSION = 7u;
const uint GBUFFER_VIEW_LIGHT_CLUSTERS = 8u;
const uint GBUFFER_VIEW_CUSTOM = 9u;

// Must match TonemapPushConstants in engine/renderer/vulkan/tonemap_pass.cpp.
layout(push_constant) uniform TonemapConstants
{
    uint gbufferView;
    // 1 for HDR10 output: GT7's HDR curve for peakNits, written relative to kUiWhiteNits.
    uint hdrOutput;
    float peakNits;
    uint unused;
    // Auto white balance, linear Rec.709 to linear Rec.709, as three columns (xyz used).
    vec4 whiteBalance[3];
}
constants;

layout(location = 0) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

void main()
{
    vec3 color;

    if (constants.gbufferView == GBUFFER_VIEW_ALBEDO)
    {
        // Sampled through the _SRGB format, so already linear; the sRGB LDR target re-encodes it.
        // Shows albedo as stored: unshaded, unexposed and not tone mapped.
        color = texture(gbufferAlbedo, fragTexCoord).rgb;
    }
    else if (constants.gbufferView == GBUFFER_VIEW_NORMAL)
    {
        // World-space shading normal mapped to [0, 1]. Unwritten pixels decode to +Z and read as
        // blue.
        color = DecodeNormalOctahedral(texture(gbufferNormal, fragTexCoord).rg) * 0.5 + 0.5;
    }
    else if (constants.gbufferView == GBUFFER_VIEW_GEOMETRIC_NORMAL)
    {
        // The interpolated, face-flipped vertex normal the shadow lookup offsets along.
        color = DecodeNormalOctahedral(texture(gbufferNormal, fragTexCoord).ba) * 0.5 + 0.5;
    }
    else if (constants.gbufferView == GBUFFER_VIEW_SURFACE)
    {
        // r = metallic, g = roughness, b = occlusion.
        color = texture(gbufferSurface, fragTexCoord).rgb;
    }
    else if (constants.gbufferView == GBUFFER_VIEW_EMISSIVE)
    {
        // Emissive is pre-exposed radiance, so it is tone mapped exactly as the shaded image is.
        vec3 emissive = min(texture(gbufferEmissive, fragTexCoord).rgb, vec3(65504.0));
        color = TonemapFrameBufferRec709(emissive);
    }
    else if (constants.gbufferView == GBUFFER_VIEW_MOTION_VECTORS)
    {
        // Mid-grey is still. Red grows with rightward motion and green with downward motion,
        // saturating at 4 pixels per frame: the editor runs at hundreds of frames per second, where
        // walking the camera moves a surface only a pixel or two per frame. Background pixels hold
        // no vector and read grey.
        vec2 pixels = texture(gbufferVelocity, fragTexCoord).rg * vec2(textureSize(gbufferVelocity, 0));
        color = vec3(clamp(0.5 + pixels / 8.0, 0.0, 1.0), 0.5);
    }
    else if (constants.gbufferView == GBUFFER_VIEW_AMBIENT_OCCLUSION)
    {
        // The resolved screen-space AO alone, white where nothing occludes.
        color = vec3(texture(sceneAo, fragTexCoord).r);
    }
    else if (constants.gbufferView == GBUFFER_VIEW_CUSTOM)
    {
        // GB5 as stored. For clearcoat, red is the coat's factor and green its roughness; for sheen,
        // rgb is its colour (its roughness, in alpha, does not show). Default Lit pixels are black.
        color = texture(gbufferCustom, fragTexCoord).rgb;
    }
    else if (constants.gbufferView == GBUFFER_VIEW_LIGHT_CLUSTERS)
    {
        // The lighting pass wrote heat colours times kFrameBufferUnitsPerExposed; scaling back shows
        // them as they were meant, without the operator bending their hues. Blend surfaces, shaded
        // on top by the forward pass, are exposed but not tone mapped here.
        color = min(texture(hdrTexture, fragTexCoord).rgb * kExposedPerFrameBufferUnit, vec3(1.0));
    }
    else
    {
        // The HDR target already holds pre-exposed values in the operator's unit (see
        // pre_exposure.glsl). Clamp below fp16's maximum before the operator: an infinite input
        // would turn into NaN inside it and show a very bright pixel as black.
        color = min(texture(hdrTexture, fragTexCoord).rgb, vec3(65504.0));

        // Auto white balance, before the operator: a partial chromatic adaptation toward D65 (see
        // engine/renderer/white_balance.h). It can push a saturated colour slightly negative; the
        // operator clamps at zero.
        color = mat3(constants.whiteBalance[0].xyz, constants.whiteBalance[1].xyz, constants.whiteBalance[2].xyz) * color;

        // GT7's operator (see gt7_tonemap.glsl). The result is display-referred linear Rec.709;
        // the LDR target's sRGB format applies the transfer function on write.
        if (constants.hdrOutput != 0u)
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
    outColor = vec4(color, 1.0);
}
