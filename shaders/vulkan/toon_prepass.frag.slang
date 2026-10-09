#include "specialization.slang"

// The toon prepass (toon_pass.h): an anime character's linear view depth, the depth texture the toon
// pass's rim light and contact shadow read, and the eye mask. As Unity's depth-only pass, the face
// is written a little farther than it is, so the nose and the hair on the face cast the contact
// shadow and not the face onto itself.
SPECIALIZATION_BOOL(0, kOutline);

#include "toon_common.slang"

struct FragmentInput
{
    // toon.vert's outputs, all of them in its order: Direct3D matches the stages by signature
    // position (Vulkan by location), so the fragment stage declares what the vertex stage writes.
    float4 fragCoord : SV_Position;
    [[vk::location(0)]] float3 fragWorldPosition : TEXCOORD0;
    [[vk::location(1)]] float3 fragWorldNormal : TEXCOORD1;
    [[vk::location(2)]] float2 fragTexCoord : TEXCOORD2;
    [[vk::location(3)]] float2 fragTexCoord1 : TEXCOORD3;
    [[vk::location(4)]] float3 fragSmoothNormal : TEXCOORD4;
    [[vk::location(5)]] float3 fragGeometryNormal : TEXCOORD5;
    [[vk::location(6)]] nointerpolation uint fragToonIndex : TEXCOORD6;
    [[vk::location(7)]] float4 fragCurrClip : TEXCOORD7;
    [[vk::location(8)]] float4 fragPrevClip : TEXCOORD8;
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
    LoadMaterialSamplers(material.samplerIndices);
    float3 toCamera = normalize(ubo.cameraWorldPosition.xyz - input.fragWorldPosition);
    float baseAlpha = toonBaseMap.SampleBias(toonBaseMapSampler, input.fragTexCoord, MATERIAL_MIP_BIAS).a * material.baseColor.a;
    ToonAlpha(material, baseAlpha, input.fragTexCoord, toCamera, 0.0, ToonHas(material, TOON_FEATURE_ALPHA_TEST));

    float faceMask = ToonFaceMask(material, input.fragTexCoord);
    output.outLinearDepth = -mul(ubo.view, float4(input.fragWorldPosition, 1.0)).z + faceMask * material.depthTexShadowTintFace.w;
    output.outMask = ToonHas(material, TOON_FEATURE_STENCIL_WRITE) ? 1.0 : 0.0;
    return output;
}
