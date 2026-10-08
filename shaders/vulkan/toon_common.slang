#ifndef TOON_COMMON_SLANG
#define TOON_COMMON_SLANG

// The anime character shading of AnimateApp's "Universal Render Pipeline/Anime/Character" (a NiloToon
// clone), as its DXBC reads (docs/design/2026-10-07-yuki-toon-shading-design.md): what toon.vert,
// toon.frag and toon_prepass.frag share. The C++ side is engine/scene/toon_material.h and
// engine/renderer/vulkan/toon_pass.h.

#include "scene_common.slang"

// kToonFeature* in engine/scene/toon_material.h.
static const uint TOON_FEATURE_FACE = 1u << 0;
static const uint TOON_FEATURE_FACE_MASK = 1u << 1;
static const uint TOON_FEATURE_FACE_SDF = 1u << 2;
static const uint TOON_FEATURE_SKIN = 1u << 3;
static const uint TOON_FEATURE_SKIN_MASK = 1u << 4;
static const uint TOON_FEATURE_SHADOW_COLOR = 1u << 5;
static const uint TOON_FEATURE_OCCLUSION = 1u << 6;
static const uint TOON_FEATURE_SHADING_GRADE = 1u << 7;
static const uint TOON_FEATURE_ADDITIVE_MATCAP = 1u << 8;
static const uint TOON_FEATURE_ALPHA_BLEND_MATCAP = 1u << 9;
static const uint TOON_FEATURE_ALPHA_OVERRIDE = 1u << 10;
static const uint TOON_FEATURE_ALPHA_TEST = 1u << 11;
static const uint TOON_FEATURE_OUTLINE_WIDTH_TEXTURE = 1u << 12;
static const uint TOON_FEATURE_SELF_SHADOW = 1u << 13;
static const uint TOON_FEATURE_OUTLINE = 1u << 14;
static const uint TOON_FEATURE_DEPTH_TEX_RIM_SHADOW = 1u << 15;
static const uint TOON_FEATURE_TRANSPARENT = 1u << 16;
static const uint TOON_FEATURE_STENCIL_WRITE = 1u << 17;
static const uint TOON_FEATURE_STENCIL_NOT_EQUAL = 1u << 18;
static const uint TOON_FEATURE_STENCIL_EQUAL = 1u << 19;

// GpuToonMaterial, member for member (std430, 29 x float4); the comments there name every component.
struct ToonMaterial
{
    float4 baseColor;
    float4 mainLight;
    float4 occlusion;
    float4 shadingGrade;
    float4 shadingGrade2;
    float4 shadowTint;
    float4 shadowHsv;
    float4 skinShadowTint;
    float4 faceShadowTint;
    float4 alphaOverride;
    float4 alphaBlendMatCapTint;
    float4 additiveMatCap;
    float4 additiveMatCap2;
    float4 rimLight;
    float4 depthTexRim;
    float4 depthTexShadow;
    float4 depthTexShadowTint;
    float4 depthTexShadowTintFace;
    float4 depthTexRim2;
    float4 faceSdf;
    float4 faceSdf2;
    float4 outline;
    float4 outlineTint;
    float4 outlineSkinOverride;
    float4 headPosition;
    float4 headForward;
    float4 headUp;
    float4 characterRim;
    uint4 features;
};

// VulkanToonMaterials: the frame's toon materials, indexed by the draw's toonIndex.
[[vk::binding(0, 2)]] StructuredBuffer<ToonMaterial> toonMaterials;

// ToonPushConstants in engine/renderer/vulkan/toon_pass.h.
struct DrawConstants
{
    float4x4 model;
    uint toonIndex;
    // 2 to the toon exposure (RenderDebugSettings::toonExposureEv).
    float exposureScale;
};
[[vk::push_constant]] ConstantBuffer<DrawConstants> drawData;

// The toon maps, each in the PBR slot of the material set (set 1) that ReadToonMaterial
// (gltf_model_loader.cpp) puts it in, so each is decoded in its colour space.
[[vk::binding(0, 1)]] Sampler2D toonBaseMap;                 // _BaseMap (sRGB)
[[vk::binding(2, 1)]] Sampler2D toonShadingGradeMap;         // _ShadingGradeMap, r
[[vk::binding(3, 1)]] Sampler2D toonAdditiveMatCapMask;      // _AdditiveMatCapMask, g
[[vk::binding(4, 1)]] Sampler2D toonOcclusionMap;            // _OcclusionMap, g
[[vk::binding(5, 1)]] Sampler2D toonAdditiveMatCap;          // _AdditiveMatCap (sRGB)
[[vk::binding(13, 1)]] Sampler2D toonAlphaBlendMatCapMask;   // _AlphaBlendMatCapMask, g
[[vk::binding(14, 1)]] Sampler2D toonFaceMaskMap;            // _FaceMaskMap, g
[[vk::binding(15, 1)]] Sampler2D toonAlphaBlendMatCap;       // _AlphaBlendMatCap (sRGB)
[[vk::binding(16, 1)]] Sampler2D toonSkinMaskMap;            // _SkinMaskMap, g
[[vk::binding(18, 1)]] Sampler2D toonFaceShadowGradientMap;  // _FaceShadowGradientMap, g (UV 1)
[[vk::binding(21, 1)]] Sampler2D toonFaceShadowGradientMask; // _FaceShadowGradientMaskMap, g (UV 1)
[[vk::binding(22, 1)]] Sampler2D toonOutlineWidthMap;        // _OutlineWidthTexture, g
[[vk::binding(23, 1)]] Sampler2D toonAlphaOverrideMap;       // _AlphaOverrideMap, g

bool ToonHas(ToonMaterial material, uint feature)
{
    return (material.features.x & feature) != 0u;
}

// The face mask: 1 for the whole of a _FACE material without one, the map (optionally inverted)
// with it, 0 for anything else. Read without mips, as the shader does.
float ToonFaceMask(ToonMaterial material, float2 uv)
{
    if (!ToonHas(material, TOON_FEATURE_FACE))
    {
        return 0.0;
    }
    if (!ToonHas(material, TOON_FEATURE_FACE_MASK))
    {
        return 1.0;
    }
    float mask = toonFaceMaskMap.SampleLevel(uv, 0.0).g;
    return material.shadingGrade2.z != 0.0 ? 1.0 - mask : mask;
}

// The head's frame in world space: the bone's position and the face's forward and up.
float3 ToonHeadPosition(ToonMaterial material)
{
    return mul(drawData.model, float4(material.headPosition.xyz, 1.0)).xyz;
}

float3 ToonFaceForward(ToonMaterial material)
{
    return normalize(mul((float3x3)drawData.model, material.headForward.xyz));
}

float3 ToonFaceUp(ToonMaterial material)
{
    return normalize(mul((float3x3)drawData.model, material.headUp.xyz));
}

// The rest samples with the material mip bias and may discard: fragment stages only.
#ifndef TOON_VERTEX_STAGE

// The alpha, after the alpha override (a lerp toward the override map's g, stronger as the face turns
// to the camera) and the matcap's brightness; discards below the cutoff as _ALPHATEST_ON does, before
// and after the matcap is added.
float ToonAlpha(ToonMaterial material, float baseAlpha, float2 uv, float3 toCamera, float additiveLuminance, bool alphaTest)
{
    float alpha = baseAlpha;
    if (ToonHas(material, TOON_FEATURE_ALPHA_OVERRIDE))
    {
        float strength = material.occlusion.w;
        if (material.alphaOverride.x > 0.0)
        {
            float facing = clamp(
                (dot(ToonFaceForward(material), toCamera) - material.alphaOverride.y) / max(material.alphaOverride.z - material.alphaOverride.y, 1e-4),
                0.0, 1.0);
            facing *= facing;
            strength *= lerp(1.0, facing * facing, material.alphaOverride.x);
        }
        alpha = lerp(alpha, toonAlphaOverrideMap.SampleBias(uv, MATERIAL_MIP_BIAS).g, strength);
    }
    if (alphaTest && alpha < material.mainLight.w)
    {
        discard;
    }
    alpha = clamp(alpha + additiveLuminance, 0.0, 1.0);
    if (alphaTest && alpha < material.mainLight.w)
    {
        discard;
    }
    return alpha;
}

// The matcap lookup: the normal in a frame around the direction to the camera, its right from the
// camera's up (the shader's basis, which bends with perspective), into the map's [0, 1] square with
// the image's top at v = 0.
float2 ToonMatCapUv(float3 N, float3 toCamera)
{
    float3 cameraUp = ubo.view[1].xyz;
    float3 right = normalize(cross(cameraUp, toCamera));
    float3 up = cross(toCamera, right);
    return float2(0.5 + 0.5 * dot(N, right), 0.5 - 0.5 * dot(N, up));
}

// The albedo the shading starts from: the base map times the base colour, with the alpha-blended
// matcap over it.
float3 ToonAlbedo(ToonMaterial material, float3 baseColor, float2 uv, float3 N, float3 toCamera)
{
    float3 albedo = baseColor * material.baseColor.rgb;
    if (ToonHas(material, TOON_FEATURE_ALPHA_BLEND_MATCAP))
    {
        float mask = toonAlphaBlendMatCapMask.SampleBias(uv, MATERIAL_MIP_BIAS).g;
        mask = material.shadingGrade2.w != 0.0 ? 1.0 - mask : mask;
        float3 matcap = toonAlphaBlendMatCap.SampleBias(ToonMatCapUv(N, toCamera), MATERIAL_MIP_BIAS).rgb * material.alphaBlendMatCapTint.rgb;
        albedo = lerp(albedo, matcap, mask * material.alphaOverride.w);
    }
    return albedo;
}

// The additive matcap's light, to add to the albedo.
float3 ToonAdditiveMatCap(ToonMaterial material, float3 albedo, float2 uv, float3 N, float3 toCamera)
{
    if (!ToonHas(material, TOON_FEATURE_ADDITIVE_MATCAP))
    {
        return float3(0.0);
    }
    float3 matcap = pow(abs(toonAdditiveMatCap.SampleBias(ToonMatCapUv(N, toCamera), MATERIAL_MIP_BIAS).rgb), float3(1.0 + material.additiveMatCap2.z));
    matcap *= material.additiveMatCap.rgb * lerp(float3(1.0), albedo, material.additiveMatCap2.w);
    float mask = clamp(
        (toonAdditiveMatCapMask.SampleBias(uv, MATERIAL_MIP_BIAS).g - material.additiveMatCap2.x) /
            max(material.additiveMatCap2.y - material.additiveMatCap2.x, 1e-4),
        0.0, 1.0);
    return matcap * mask * material.additiveMatCap.a;
}

// The cel shading of the main light: N.L through the ramp, which the shading grade map moves.
float ToonCelShade(ToonMaterial material, float NdotL, float2 uv)
{
    float offset = 0.0;
    if (ToonHas(material, TOON_FEATURE_SHADING_GRADE))
    {
        float grade = toonShadingGradeMap.SampleBias(uv, MATERIAL_MIP_BIAS).r;
        grade = material.shadingGrade2.y != 0.0 ? 1.0 - grade : grade;
        grade = clamp((grade - material.shadingGrade.x) / max(material.shadingGrade.y - material.shadingGrade.x, 1e-4), 0.0, 1.0);
        offset = (grade - 0.5) * material.shadingGrade.z * material.shadingGrade.w + material.shadingGrade2.x;
    }
    float lo = material.mainLight.x + offset;
    float hi = material.mainLight.y + offset;
    float cel = smoothstep(0.0, 1.0, clamp((NdotL - lo) / max(hi - lo, 1e-5), 0.0, 1.0));
    return lerp(cel, 1.0, material.mainLight.z);
}

// RGB to HSV and back, as the shader computes them (Sam Hocevar's branchless forms).
float3 ToonRgbToHsv(float3 c)
{
    float4 K = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    float4 p = c.g >= c.b ? float4(c.gb, K.xy) : float4(c.bg, K.wz);
    float4 q = c.r >= p.x ? float4(c.r, p.yzx) : float4(p.xyw, c.r);
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-4;
    return float3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

float3 ToonHsvToRgb(float3 c)
{
    float3 p = abs(frac(c.xxx + float3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return c.z * lerp(float3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);
}

// What the albedo becomes in shadow: moved in hue, saturation and value and tinted (_SHADOW_COLOR),
// or the albedo itself; then, on skin, the albedo tinted by the skin's shadow colour, the face's
// where the face mask is.
float3 ToonShadowColor(ToonMaterial material, float3 albedo, float faceMask, float skinMask)
{
    float3 shadow = albedo;
    if (ToonHas(material, TOON_FEATURE_SHADOW_COLOR))
    {
        float strength = material.shadowTint.a;
        float3 hsv = ToonRgbToHsv(albedo);
        hsv.x += material.shadowHsv.x * strength;
        float boosted = pow(hsv.y, 1.0 / (1.0 + 3.0 * material.shadowHsv.y * strength));
        hsv.y = lerp(hsv.y, boosted, smoothstep(0.0, 1.0, clamp(hsv.y * 4.0, 0.0, 1.0)));
        hsv.z *= lerp(1.0, material.shadowHsv.z, strength);
        shadow = ToonHsvToRgb(hsv) * material.shadowTint.rgb;
    }
    if (ToonHas(material, TOON_FEATURE_SKIN))
    {
        float3 skin = albedo * lerp(material.skinShadowTint.rgb, material.faceShadowTint.rgb, faceMask);
        shadow = lerp(shadow, skin, skinMask);
    }
    return shadow;
}

float ToonSkinMask(ToonMaterial material, float2 uv)
{
    if (!ToonHas(material, TOON_FEATURE_SKIN))
    {
        return 0.0;
    }
    return ToonHas(material, TOON_FEATURE_SKIN_MASK) ? toonSkinMaskMap.SampleLevel(uv, 0.0).g : 1.0;
}

#endif

#endif
