#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace me
{

// An anime character material (glTF MINIENGINE_toon): the parameters of AnimateApp's "Universal
// Render Pipeline/Anime/Character" shader, a NiloToon clone, which the toon passes
// (engine/renderer/vulkan/toon_pass.h) shade it by instead of the PBR model. The shader's
// keywords become these feature bits; each is a branch of shaders/vulkan/toon_common.glsl, which
// declares the same values as TOON_FEATURE_*. See docs/design/2026-10-07-yuki-toon-shading-design.md.
// _FACE: the material is (part of) a face. With FaceMask only where the face mask map says so.
inline constexpr uint32_t kToonFeatureFace = 1u << 0;
inline constexpr uint32_t kToonFeatureFaceMask = 1u << 1;
// _FACE_SHADOW_GRADIENTMAP: the face's shadow from a signed distance map on TEXCOORD_1.
inline constexpr uint32_t kToonFeatureFaceSdf = 1u << 2;
// _SKIN, _SKIN_MASK: the shadow is the albedo tinted by the skin (or face) shadow colour.
inline constexpr uint32_t kToonFeatureSkin = 1u << 3;
inline constexpr uint32_t kToonFeatureSkinMask = 1u << 4;
// _SHADOW_COLOR: the shadow is the albedo moved in hue, saturation and value, then tinted.
inline constexpr uint32_t kToonFeatureShadowColor = 1u << 5;
inline constexpr uint32_t kToonFeatureOcclusion = 1u << 6;
inline constexpr uint32_t kToonFeatureShadingGrade = 1u << 7;
inline constexpr uint32_t kToonFeatureAdditiveMatCap = 1u << 8;
inline constexpr uint32_t kToonFeatureAlphaBlendMatCap = 1u << 9;
inline constexpr uint32_t kToonFeatureAlphaOverride = 1u << 10;
inline constexpr uint32_t kToonFeatureAlphaTest = 1u << 11;
inline constexpr uint32_t kToonFeatureOutlineWidthTexture = 1u << 12;
// _EnableSelfShadow: the sun's shadow darkens the cel shading.
inline constexpr uint32_t kToonFeatureSelfShadow = 1u << 13;
// The outline pass is not disabled: an inverted hull is drawn around the surface.
inline constexpr uint32_t kToonFeatureOutline = 1u << 14;
// _DEPTHTEX_RIMLIGHT_SHADOW: rim light and contact shadow from the characters' linear depth.
inline constexpr uint32_t kToonFeatureDepthTexRimShadow = 1u << 15;
// _SurfaceType other than opaque: blended over what is behind it with its alpha.
inline constexpr uint32_t kToonFeatureTransparent = 1u << 16;
// The stencil the shader writes and tests, which the toon mask target stands in for: the eyes mark
// it (stencil pass Replace), the front hair skips it (NotEqual) and its redraw keeps to it (Equal).
inline constexpr uint32_t kToonFeatureStencilWrite = 1u << 17;
inline constexpr uint32_t kToonFeatureStencilNotEqual = 1u << 18;
inline constexpr uint32_t kToonFeatureStencilEqual = 1u << 19;

// One toon material as the toon passes read it (ToonMaterial in shaders/vulkan/toon_common.glsl),
// std430, 29 x vec4. Colours are linear. Model-space vectors (the head) follow the entity's
// transform in the shader.
struct alignas(16) GpuToonMaterial
{
    float baseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    // x, y = _MainLightRamp (the cel edge's start and end in N.L), z = _MainLightIgnoreCelShade, w = _Cutoff.
    float mainLight[4] = {-0.05f, 0.05f, 0.0f, 0.5f};
    // x, y = _OcclusionRemapStart/End, z = _OcclusionStrength, w = _AlphaOverrideStrength.
    float occlusion[4] = {0.0f, 1.0f, 1.0f, 1.0f};
    // x, y = _ShadingGradeMapRemapStart/End, z = _ShadingGradeMapStrength, w = _ShadingGradeMapApplyRange.
    float shadingGrade[4] = {0.0f, 1.0f, 1.0f, 1.0f};
    // x = _ShadingGradeMapMidPointOffset, y = _ShadingGradeMapInvertColor, z = _FaceMaskMapInvertColor,
    // w = _AlphaBlendMatCapMaskInvert.
    float shadingGrade2[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // rgb = _ShadowTint, a = _ShadowHSVStrength.
    float shadowTint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    // x = _ShadowHueOffset, y = _ShadowSaturationBoost, z = _ShadowValueMultiply, w = _SelfShadowIntensity.
    float shadowHsv[4] = {0.0f, 0.2f, 0.7f, 1.0f};
    // rgb = _SkinShadowTintColor, a = _SelfShadowIntensityForNonFace.
    float skinShadowTint[4] = {1.0f, 0.6f, 0.6f, 1.0f};
    // rgb = _FaceShadowTintColor, a = _SelfShadowIntensityForFace.
    float faceShadowTint[4] = {1.0f, 0.8f, 0.8f, 0.0f};
    // x = _ApplyAlphaOverrideOnlyWhenFaceForwardIsPointingToCamera, y, z = its remap start and end,
    // w = _AlphaBlendMatCapStrength.
    float alphaOverride[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    float alphaBlendMatCapTint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    // rgb = _AdditiveMatCapTint, a = _AdditiveMatCapIntensity.
    float additiveMatCap[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    // x, y = _AdditiveMatCapMaskRemapStart/End, z = _AdditiveMatCapExtractBrightArea,
    // w = _AdditiveMatCapMixWithBaseMapColor.
    float additiveMatCap2[4] = {0.0f, 1.0f, 0.0f, 0.5f};
    // rgb = _RimLightColor, a = _RimLightIntensity.
    float rimLight[4] = {1.0f, 1.0f, 1.0f, 1.5f};
    // x = _RimLightMixWithBaseMap, y = _DepthTexRimLightAndShadowWidthMultiplier,
    // z = _DepthTexRimLightWidthMultiplier, w = _DepthTexRimLightFixDottedLineArtifactsExtendMultiplier.
    float depthTexRim[4] = {0.5f, 0.5f, 1.0f, 0.1f};
    // x = _DepthTexShadowWidthMultiplier, y = _DepthTexShadowUsage, z = _DepthTexShadowIgnoreLightDir,
    // w = _DepthTexShadowBrightness.
    float depthTexShadow[4] = {1.0f, 1.0f, 0.0f, 0.85f};
    // rgb = _DepthTexShadowTintColor, a = _DepthTexShadowBrightnessForFace.
    float depthTexShadowTint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    // rgb = _DepthTexShadowTintColorForFace, a = _FaceAreaCameraDepthTextureZWriteOffset (metres).
    float depthTexShadowTintFace[4] = {1.0f, 0.7f, 0.7f, 0.04f};
    // x = _DepthTexRimLightThresholdOffset, y = _DepthTexRimLightFadeoutRange, z = _PerCharZOffset;
    // w unused.
    float depthTexRim2[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    // x = _FaceShadowGradientMapFaceMidPoint, y = _FaceShadowGradientIntensity,
    // z = _FaceShadowGradientOffset, w = _FaceShadowGradientResultSoftness.
    float faceSdf[4] = {0.5f, 1.0f, 0.1f, 0.005f};
    // x = _FaceShadowGradientRemoveGeometryShadow, y = the face normal fix's amount (the material's
    // times the character's), z = its method (0 flatten to the face's forward, 1 a proxy sphere
    // around the head); w unused.
    float faceSdf2[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // x = _OutlineWidth (times its extra multiplier), y = _OutlineZOffset, z = _OutlineZOffsetForFaceArea
    // (both metres pushed away from the camera); w unused.
    float outline[4] = {0.5f, 0.0001f, 0.02f, 0.0f};
    // rgb = _OutlineTintColor; a unused.
    float outlineTint[4] = {0.25f, 0.25f, 0.25f, 1.0f};
    // rgb = _OutlineTintColorSkinAreaOverride, a = how much skin takes it.
    float outlineSkinOverride[4] = {0.4f, 0.2f, 0.2f, 1.0f};
    // xyz = the head bone's position, w = the character rim's power.
    float headPosition[4] = {0.0f, 0.0f, 0.0f, 4.0f};
    // xyz = the face's forward and up, unit length.
    float headForward[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    float headUp[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    // rgb = the rim over the whole character, times pow(1 - N.V, headPosition.w); a unused.
    float characterRim[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // x = kToonFeature* bits, y = the render queue; zw unused.
    uint32_t features[4] = {0u, 2000u, 0u, 0u};
};

static_assert(sizeof(GpuToonMaterial) == 29 * 16, "GpuToonMaterial must stay 29 x vec4 to match ToonMaterial in toon_common.glsl");
static_assert(offsetof(GpuToonMaterial, features) == 28 * 16, "features must be the last vec4");

// What a toon material carries beside its PBR fallback. Its maps sit in the PBR texture slots the
// toon shaders read them from (ToonTextureSlot), each in a slot of the right colour space.
struct ToonMaterialData
{
    GpuToonMaterial gpu;
    // The head bone's node name; on a skinned mesh the face's frame (gpu.headPosition and the rest,
    // at the bind pose) follows that joint as the model animates.
    std::string headNode;

    uint32_t Features() const
    {
        return gpu.features[0];
    }
    bool Has(uint32_t feature) const
    {
        return (gpu.features[0] & feature) != 0u;
    }
    // Unity's render queue: 2000 opaque, 2450 and up alpha-tested and later; the toon pass draws the
    // transparent ones in this order.
    uint32_t RenderQueue() const
    {
        return gpu.features[1];
    }
};
}
