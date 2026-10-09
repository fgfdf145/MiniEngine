#include "specialization.slang"

// The toon prepass (toon_pass.h): an anime character's linear view depth, the depth texture the toon
// pass's rim light and contact shadow read, and the eye mask. As Unity's depth-only pass, the face
// is written a little farther than it is, so the nose and the hair on the face cast the contact
// shadow and not the face onto itself.
SPECIALIZATION_BOOL(0, kOutline);

#include "toon_common.slang"

struct FragmentInput
{
    [[vk::location(0)]] float3 fragWorldPosition : TEXCOORD0;
    [[vk::location(2)]] float2 fragTexCoord : TEXCOORD2;
    [[vk::location(6)]] nointerpolation uint fragToonIndex : TEXCOORD6;
};

struct FragmentOutput
{
    [[vk::location(0)]] float outLinearDepth : SV_Target0;
    [[vk::location(1)]] float outMask : SV_Target1;
};

[shader("fragment")]
FragmentOutput main(FragmentInput input)
{
    FragmentOutput output;
    ToonMaterial material = toonMaterials[input.fragToonIndex];
    float3 toCamera = normalize(ubo.cameraWorldPosition.xyz - input.fragWorldPosition);
    float baseAlpha = toonBaseMap.SampleBias(toonBaseMapSampler, input.fragTexCoord, MATERIAL_MIP_BIAS).a * material.baseColor.a;
    ToonAlpha(material, baseAlpha, input.fragTexCoord, toCamera, 0.0, ToonHas(material, TOON_FEATURE_ALPHA_TEST));

    float faceMask = ToonFaceMask(material, input.fragTexCoord);
    output.outLinearDepth = -mul(ubo.view, float4(input.fragWorldPosition, 1.0)).z + faceMask * material.depthTexShadowTintFace.w;
    output.outMask = ToonHas(material, TOON_FEATURE_STENCIL_WRITE) ? 1.0 : 0.0;
    return output;
}
