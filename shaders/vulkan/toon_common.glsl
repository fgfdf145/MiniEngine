#ifndef TOON_COMMON_GLSL
#define TOON_COMMON_GLSL

// The anime character shading of AnimateApp's "Universal Render Pipeline/Anime/Character" (a NiloToon
// clone), as its DXBC reads (docs/design/2026-10-07-yuki-toon-shading-design.md): what toon.vert,
// toon.frag and toon_prepass.frag share. The C++ side is engine/scene/toon_material.h and
// engine/renderer/vulkan/toon_pass.h.

#include "scene_common.glsl"

// kToonFeature* in engine/scene/toon_material.h.
const uint TOON_FEATURE_FACE = 1u << 0;
const uint TOON_FEATURE_FACE_MASK = 1u << 1;
const uint TOON_FEATURE_FACE_SDF = 1u << 2;
const uint TOON_FEATURE_SKIN = 1u << 3;
const uint TOON_FEATURE_SKIN_MASK = 1u << 4;
const uint TOON_FEATURE_SHADOW_COLOR = 1u << 5;
const uint TOON_FEATURE_OCCLUSION = 1u << 6;
const uint TOON_FEATURE_SHADING_GRADE = 1u << 7;
const uint TOON_FEATURE_ADDITIVE_MATCAP = 1u << 8;
const uint TOON_FEATURE_ALPHA_BLEND_MATCAP = 1u << 9;
const uint TOON_FEATURE_ALPHA_OVERRIDE = 1u << 10;
const uint TOON_FEATURE_ALPHA_TEST = 1u << 11;
const uint TOON_FEATURE_OUTLINE_WIDTH_TEXTURE = 1u << 12;
const uint TOON_FEATURE_SELF_SHADOW = 1u << 13;
const uint TOON_FEATURE_OUTLINE = 1u << 14;
const uint TOON_FEATURE_DEPTH_TEX_RIM_SHADOW = 1u << 15;
const uint TOON_FEATURE_TRANSPARENT = 1u << 16;
const uint TOON_FEATURE_STENCIL_WRITE = 1u << 17;
const uint TOON_FEATURE_STENCIL_NOT_EQUAL = 1u << 18;
const uint TOON_FEATURE_STENCIL_EQUAL = 1u << 19;

// GpuToonMaterial, member for member (std430, 29 x vec4); the comments there name every component.
struct ToonMaterial
{
    vec4 baseColor;
    vec4 mainLight;
    vec4 occlusion;
    vec4 shadingGrade;
    vec4 shadingGrade2;
    vec4 shadowTint;
    vec4 shadowHsv;
    vec4 skinShadowTint;
    vec4 faceShadowTint;
    vec4 alphaOverride;
    vec4 alphaBlendMatCapTint;
    vec4 additiveMatCap;
    vec4 additiveMatCap2;
    vec4 rimLight;
    vec4 depthTexRim;
    vec4 depthTexShadow;
    vec4 depthTexShadowTint;
    vec4 depthTexShadowTintFace;
    vec4 depthTexRim2;
    vec4 faceSdf;
    vec4 faceSdf2;
    vec4 outline;
    vec4 outlineTint;
    vec4 outlineSkinOverride;
    vec4 headPosition;
    vec4 headForward;
    vec4 headUp;
    vec4 characterRim;
    uvec4 features;
};

// VulkanToonMaterials: the frame's toon materials, indexed by the draw's toonIndex.
layout(set = 2, binding = 0, std430) readonly buffer ToonMaterialBuffer
{
    ToonMaterial materials[];
}
toonMaterials;

// ToonPushConstants in engine/renderer/vulkan/toon_pass.h.
layout(push_constant) uniform DrawConstants
{
    mat4 model;
    uint toonIndex;
    // 2 to the toon exposure (RenderDebugSettings::toonExposureEv).
    float exposureScale;
}
drawData;

// The toon maps, each in the PBR slot of the material set (set 1) that ReadToonMaterial
// (gltf_model_loader.cpp) puts it in, so each is decoded in its colour space.
layout(set = 1, binding = 0) uniform sampler2D toonBaseMap;                 // _BaseMap (sRGB)
layout(set = 1, binding = 2) uniform sampler2D toonShadingGradeMap;         // _ShadingGradeMap, r
layout(set = 1, binding = 3) uniform sampler2D toonAdditiveMatCapMask;      // _AdditiveMatCapMask, g
layout(set = 1, binding = 4) uniform sampler2D toonOcclusionMap;            // _OcclusionMap, g
layout(set = 1, binding = 5) uniform sampler2D toonAdditiveMatCap;          // _AdditiveMatCap (sRGB)
layout(set = 1, binding = 13) uniform sampler2D toonAlphaBlendMatCapMask;   // _AlphaBlendMatCapMask, g
layout(set = 1, binding = 14) uniform sampler2D toonFaceMaskMap;            // _FaceMaskMap, g
layout(set = 1, binding = 15) uniform sampler2D toonAlphaBlendMatCap;       // _AlphaBlendMatCap (sRGB)
layout(set = 1, binding = 16) uniform sampler2D toonSkinMaskMap;            // _SkinMaskMap, g
layout(set = 1, binding = 18) uniform sampler2D toonFaceShadowGradientMap;  // _FaceShadowGradientMap, g (UV 1)
layout(set = 1, binding = 21) uniform sampler2D toonFaceShadowGradientMask; // _FaceShadowGradientMaskMap, g (UV 1)
layout(set = 1, binding = 22) uniform sampler2D toonOutlineWidthMap;        // _OutlineWidthTexture, g
layout(set = 1, binding = 23) uniform sampler2D toonAlphaOverrideMap;       // _AlphaOverrideMap, g

bool ToonHas(ToonMaterial material, uint feature)
{
    return (material.features.x & feature) != 0u;
}

// The face mask: 1 for the whole of a _FACE material without one, the map (optionally inverted)
// with it, 0 for anything else. Read without mips, as the shader does.
float ToonFaceMask(ToonMaterial material, vec2 uv)
{
    if (!ToonHas(material, TOON_FEATURE_FACE))
    {
        return 0.0;
    }
    if (!ToonHas(material, TOON_FEATURE_FACE_MASK))
    {
        return 1.0;
    }
    float mask = textureLod(toonFaceMaskMap, uv, 0.0).g;
    return material.shadingGrade2.z != 0.0 ? 1.0 - mask : mask;
}

// The head's frame in world space: the bone's position and the face's forward and up.
vec3 ToonHeadPosition(ToonMaterial material)
{
    return (drawData.model * vec4(material.headPosition.xyz, 1.0)).xyz;
}

vec3 ToonFaceForward(ToonMaterial material)
{
    return normalize(mat3(drawData.model) * material.headForward.xyz);
}

vec3 ToonFaceUp(ToonMaterial material)
{
    return normalize(mat3(drawData.model) * material.headUp.xyz);
}

// The rest samples with the material mip bias and may discard: fragment stages only.
#ifndef TOON_VERTEX_STAGE

// The alpha, after the alpha override (a lerp toward the override map's g, stronger as the face turns
// to the camera) and the matcap's brightness; discards below the cutoff as _ALPHATEST_ON does, before
// and after the matcap is added.
float ToonAlpha(ToonMaterial material, float baseAlpha, vec2 uv, vec3 toCamera, float additiveLuminance, bool alphaTest)
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
            strength *= mix(1.0, facing * facing, material.alphaOverride.x);
        }
        alpha = mix(alpha, texture(toonAlphaOverrideMap, uv, MATERIAL_MIP_BIAS).g, strength);
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
vec2 ToonMatCapUv(vec3 N, vec3 toCamera)
{
    vec3 cameraUp = vec3(ubo.view[0][1], ubo.view[1][1], ubo.view[2][1]);
    vec3 right = normalize(cross(cameraUp, toCamera));
    vec3 up = cross(toCamera, right);
    return vec2(0.5 + 0.5 * dot(N, right), 0.5 - 0.5 * dot(N, up));
}

// The albedo the shading starts from: the base map times the base colour, with the alpha-blended
// matcap over it.
vec3 ToonAlbedo(ToonMaterial material, vec3 baseColor, vec2 uv, vec3 N, vec3 toCamera)
{
    vec3 albedo = baseColor * material.baseColor.rgb;
    if (ToonHas(material, TOON_FEATURE_ALPHA_BLEND_MATCAP))
    {
        float mask = texture(toonAlphaBlendMatCapMask, uv, MATERIAL_MIP_BIAS).g;
        mask = material.shadingGrade2.w != 0.0 ? 1.0 - mask : mask;
        vec3 matcap = texture(toonAlphaBlendMatCap, ToonMatCapUv(N, toCamera), MATERIAL_MIP_BIAS).rgb * material.alphaBlendMatCapTint.rgb;
        albedo = mix(albedo, matcap, mask * material.alphaOverride.w);
    }
    return albedo;
}

// The additive matcap's light, to add to the albedo.
vec3 ToonAdditiveMatCap(ToonMaterial material, vec3 albedo, vec2 uv, vec3 N, vec3 toCamera)
{
    if (!ToonHas(material, TOON_FEATURE_ADDITIVE_MATCAP))
    {
        return vec3(0.0);
    }
    vec3 matcap = pow(abs(texture(toonAdditiveMatCap, ToonMatCapUv(N, toCamera), MATERIAL_MIP_BIAS).rgb), vec3(1.0 + material.additiveMatCap2.z));
    matcap *= material.additiveMatCap.rgb * mix(vec3(1.0), albedo, material.additiveMatCap2.w);
    float mask = clamp(
        (texture(toonAdditiveMatCapMask, uv, MATERIAL_MIP_BIAS).g - material.additiveMatCap2.x) /
            max(material.additiveMatCap2.y - material.additiveMatCap2.x, 1e-4),
        0.0, 1.0);
    return matcap * mask * material.additiveMatCap.a;
}

// The cel shading of the main light: N.L through the ramp, which the shading grade map moves.
float ToonCelShade(ToonMaterial material, float NdotL, vec2 uv)
{
    float offset = 0.0;
    if (ToonHas(material, TOON_FEATURE_SHADING_GRADE))
    {
        float grade = texture(toonShadingGradeMap, uv, MATERIAL_MIP_BIAS).r;
        grade = material.shadingGrade2.y != 0.0 ? 1.0 - grade : grade;
        grade = clamp((grade - material.shadingGrade.x) / max(material.shadingGrade.y - material.shadingGrade.x, 1e-4), 0.0, 1.0);
        offset = (grade - 0.5) * material.shadingGrade.z * material.shadingGrade.w + material.shadingGrade2.x;
    }
    float lo = material.mainLight.x + offset;
    float hi = material.mainLight.y + offset;
    float cel = smoothstep(0.0, 1.0, clamp((NdotL - lo) / max(hi - lo, 1e-5), 0.0, 1.0));
    return mix(cel, 1.0, material.mainLight.z);
}

// RGB to HSV and back, as the shader computes them (Sam Hocevar's branchless forms).
vec3 ToonRgbToHsv(vec3 c)
{
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = c.g >= c.b ? vec4(c.gb, K.xy) : vec4(c.bg, K.wz);
    vec4 q = c.r >= p.x ? vec4(c.r, p.yzx) : vec4(p.xyw, c.r);
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-4;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 ToonHsvToRgb(vec3 c)
{
    vec3 p = abs(fract(c.xxx + vec3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);
}

// What the albedo becomes in shadow: moved in hue, saturation and value and tinted (_SHADOW_COLOR),
// or the albedo itself; then, on skin, the albedo tinted by the skin's shadow colour, the face's
// where the face mask is.
vec3 ToonShadowColor(ToonMaterial material, vec3 albedo, float faceMask, float skinMask)
{
    vec3 shadow = albedo;
    if (ToonHas(material, TOON_FEATURE_SHADOW_COLOR))
    {
        float strength = material.shadowTint.a;
        vec3 hsv = ToonRgbToHsv(albedo);
        hsv.x += material.shadowHsv.x * strength;
        float boosted = pow(hsv.y, 1.0 / (1.0 + 3.0 * material.shadowHsv.y * strength));
        hsv.y = mix(hsv.y, boosted, smoothstep(0.0, 1.0, clamp(hsv.y * 4.0, 0.0, 1.0)));
        hsv.z *= mix(1.0, material.shadowHsv.z, strength);
        shadow = ToonHsvToRgb(hsv) * material.shadowTint.rgb;
    }
    if (ToonHas(material, TOON_FEATURE_SKIN))
    {
        vec3 skin = albedo * mix(material.skinShadowTint.rgb, material.faceShadowTint.rgb, faceMask);
        shadow = mix(shadow, skin, skinMask);
    }
    return shadow;
}

float ToonSkinMask(ToonMaterial material, vec2 uv)
{
    if (!ToonHas(material, TOON_FEATURE_SKIN))
    {
        return 0.0;
    }
    return ToonHas(material, TOON_FEATURE_SKIN_MASK) ? textureLod(toonSkinMaskMap, uv, 0.0).g : 1.0;
}

#endif

#endif
