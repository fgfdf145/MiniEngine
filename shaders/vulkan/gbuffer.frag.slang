#include "shader_helpers.slang"
#include "scene_common.slang"
#include "gbuffer_common.slang"
#include "normal_map.slang"
#include "material_common.slang"
#include "material_uv.slang"
#include "specular_aa.slang"
#include "anisotropy_common.slang"
#include "specialization.slang"

SPECIALIZATION_BOOL(0, kAlphaMask);
// A deferred decal (MaterialPipelineSetConfig::decal): the outputs carry the base colour's alpha,
// which the pipeline blends albedo, metallic, roughness and emission by; it writes nothing else.
SPECIALIZATION_BOOL(2, kDecal);
// The draw is a Blend item (MaterialAlphaMode::Blend).
SPECIALIZATION_BOOL(3, kBlendItem);

// Compiled twice more for the path traced layer of the forward-shaded surfaces
// (VulkanPathTraceLayerPass, docs/design/2026-10-08-path-tracing-missing-effects-design.md):
// PATH_TRACE_LAYER_PASS 1 (path_trace_layer_depth.frag.spv) writes the fragment's depth alone, where a
// MAX blend keeps the nearest; 2 (path_trace_layer_surface.frag.spv) writes the G-buffer of the
// fragment at that depth alone (albedo, normals, surface, motion), for the path tracer to trace from.
#ifndef PATH_TRACE_LAYER_PASS
#define PATH_TRACE_LAYER_PASS 0
#endif
#if PATH_TRACE_LAYER_PASS != 0
// What a Blend surface must cover of a pixel to be its layer: below this its own light hardly shows
// over what is behind it, which keeps the layer then.
static const float PATH_TRACE_LAYER_MIN_ALPHA = 0.05;
#endif
#if PATH_TRACE_LAYER_PASS == 2
// The nearest forward-shaded surface's depth (the depth pass's result), set 0 binding 29.
[[vk::binding(29, 0)]] Texture2D pathTraceLayerDepth;
#endif

[[vk::binding(0, 1)]] Texture2D baseColorTexture;
[[vk::binding(MATERIAL_SAMPLER(0), 1)]] SamplerState baseColorTextureSampler;
[[vk::binding(1, 1)]] Texture2D normalTexture;
[[vk::binding(MATERIAL_SAMPLER(1), 1)]] SamplerState normalTextureSampler;
[[vk::binding(2, 1)]] Texture2D metallicTexture;
[[vk::binding(MATERIAL_SAMPLER(2), 1)]] SamplerState metallicTextureSampler;
[[vk::binding(3, 1)]] Texture2D roughnessTexture;
[[vk::binding(MATERIAL_SAMPLER(3), 1)]] SamplerState roughnessTextureSampler;
[[vk::binding(4, 1)]] Texture2D occlusionTexture;
[[vk::binding(MATERIAL_SAMPLER(4), 1)]] SamplerState occlusionTextureSampler;
[[vk::binding(5, 1)]] Texture2D emissiveTexture;
[[vk::binding(MATERIAL_SAMPLER(5), 1)]] SamplerState emissiveTextureSampler;
[[vk::binding(6, 1)]] Texture2D secondaryBaseColorTexture;
[[vk::binding(MATERIAL_SAMPLER(6), 1)]] SamplerState secondaryBaseColorTextureSampler;
[[vk::binding(7, 1)]] Texture2D secondaryNormalTexture;
[[vk::binding(MATERIAL_SAMPLER(7), 1)]] SamplerState secondaryNormalTextureSampler;
[[vk::binding(8, 1)]] Texture2D secondaryMetallicTexture;
[[vk::binding(MATERIAL_SAMPLER(8), 1)]] SamplerState secondaryMetallicTextureSampler;
[[vk::binding(9, 1)]] Texture2D secondaryRoughnessTexture;
[[vk::binding(MATERIAL_SAMPLER(9), 1)]] SamplerState secondaryRoughnessTextureSampler;
[[vk::binding(10, 1)]] Texture2D secondaryOcclusionTexture;
[[vk::binding(MATERIAL_SAMPLER(10), 1)]] SamplerState secondaryOcclusionTextureSampler;
[[vk::binding(11, 1)]] Texture2D secondaryEmissiveTexture;
[[vk::binding(MATERIAL_SAMPLER(11), 1)]] SamplerState secondaryEmissiveTextureSampler;
[[vk::binding(12, 1)]] Texture2D blendMaskTexture;
[[vk::binding(MATERIAL_SAMPLER(12), 1)]] SamplerState blendMaskTextureSampler;
#include "material_layers.slang"
#include "detail_layers.slang"

// triangle.vert also writes world position at location 4. The G-buffer does not store it; the
// lighting pass reconstructs it from depth, so the input is deliberately not declared here.
struct FragmentInput
{
    [[vk::location(0)]] float3 fragColor : TEXCOORD0;
    [[vk::location(1)]] float2 fragTexCoord : TEXCOORD1;
    [[vk::location(2)]] float3 fragWorldNormal : TEXCOORD2;
    [[vk::location(3)]] float4 fragWorldTangent : TEXCOORD3;
    [[vk::location(5)]] float4 fragCurrClip : TEXCOORD5;
    [[vk::location(6)]] float4 fragPrevClip : TEXCOORD6;
    [[vk::location(7)]] nointerpolation uint fragDrawSlot : TEXCOORD7;
    [[vk::location(8)]] float2 fragTexCoord1 : TEXCOORD8;
    [[vk::location(10)]] float3 fragObjectPosition : TEXCOORD10;
    float4 fragCoord : SV_Position;
    bool frontFacing : SV_IsFrontFace;
};

// Locations match VulkanGeometryPass::kAttachments. All five are float4 so no attachment receives
// fewer components than it has; channels the encoding table marks unused are written as stated.
// The layer's depth pass writes location 0 alone (the depth), its surface pass 0, 1, 2 and 4.
struct FragmentOutput
{
    [[vk::location(0)]] float4 outAlbedo : SV_Target0;   // GB0 R8G8B8A8_SRGB: rgb albedo, a = 1
#if PATH_TRACE_LAYER_PASS != 1
    [[vk::location(1)]] float4 outNormal : SV_Target1;   // GB1 R16G16B16A16_SFLOAT: rg shading normal, ba geometric normal, both octahedral
    [[vk::location(2)]] float4 outSurface : SV_Target2;  // GB2 R8G8B8A8_UNORM: metallic, roughness, occlusion, a = shading model
    [[vk::location(4)]] float4 outVelocity : SV_Target4; // R16G16B16A16_SFLOAT: rg current uv - previous uv, ba the coat's normal (octahedral)
#endif
#if PATH_TRACE_LAYER_PASS == 0
    [[vk::location(3)]] float4 outEmissive : SV_Target3; // GB3 B10G11R11_UFLOAT: rgb emissive
    [[vk::location(5)]] float4 outSpecular : SV_Target5; // GB5 R8G8B8A8_UNORM: rgb sqrt(dielectric F0), a dielectric F90
    [[vk::location(6)]] float4 outCoat : SV_Target6;     // GB6 R8G8B8A8_UNORM: coat factor, coat roughness, anisotropy angle, anisotropy strength
    [[vk::location(7)]] float4 outSheen : SV_Target7;    // GB7 R8G8B8A8_UNORM: sheen colour, sheen roughness
#endif
};

[shader("fragment")]
FragmentOutput main(FragmentInput input)
{
    FragmentOutput output;
    MaterialData material = materials[input.fragDrawSlot];

    // ---- Blend mask & blend weight ----------------------------------------
    float blendMask = MaterialTexture(blendMaskTexture, input.fragTexCoord).r;
    float blendWeight = clamp(
        lerp(0.0, material.nodeGraphFactors.y, clamp(material.nodeGraphFactors.x, 0.0, 1.0)) * blendMask,
        0.0, 1.0);

    // ---- Albedo -----------------------------------------------------------
    float4 primaryBaseColor = MaterialTexture(baseColorTexture, MaterialSlotUv(material, input.fragDrawSlot, 0u, input.fragTexCoord, input.fragTexCoord1));
    float4 secondaryBaseColor = MaterialTexture(secondaryBaseColorTexture, input.fragTexCoord);
    float4 sampledBaseColor = lerp(primaryBaseColor, secondaryBaseColor, blendWeight);
    float4 albedo = sampledBaseColor * float4(input.fragColor, 1.0) * material.baseColorFactor;
    albedo.rgb *= DetailLayersFactor(material, input.fragTexCoord, input.fragObjectPosition);

    if (kAlphaMask && albedo.a < material.alphaCutoff)
        discard;
#if PATH_TRACE_LAYER_PASS == 1
    if (kBlendItem && albedo.a < PATH_TRACE_LAYER_MIN_ALPHA)
        discard;
    output.outAlbedo = float4(input.fragCoord.z);
#else

    // ---- Normal -----------------------------------------------------------
    // A back face is only rasterized by a double-sided pipeline, and it is seen from the side the
    // vertex normal points away from. Mirroring the whole tangent frame, bitangent included, shades
    // it as the front face would be from the other side, with the normal map's perturbation
    // mirrored along with it. Single-sided pipelines cull back faces, so this is a no-op for them.
    float faceSign = input.frontFacing ? 1.0 : -1.0;
    float3 geoNormal = normalize(input.fragWorldNormal) * faceSign;
    float3 faceTangent = input.fragWorldTangent.xyz * faceSign;
    float3 tangent = normalize(faceTangent - geoNormal * dot(geoNormal, faceTangent));
    // cross(-N, -T) == cross(N, T), so the flip has to be applied to the bitangent explicitly.
    float3 bitangent = normalize(cross(geoNormal, tangent) * input.fragWorldTangent.w) * faceSign;
    float3x3 TBN = MatrixFromColumns(tangent, bitangent, geoNormal);

    float3 nrmPrimary = DecodeNormalMap(MaterialTexture(normalTexture, MaterialSlotUv(material, input.fragDrawSlot, 1u, input.fragTexCoord, input.fragTexCoord1)));
    nrmPrimary.xy = RotateMaterialTangentXy(material, input.fragDrawSlot, 1u, nrmPrimary.xy);
    float3 nrmSecondary = DecodeNormalMap(MaterialTexture(secondaryNormalTexture, input.fragTexCoord));
    float3 nrmSample = normalize(lerp(nrmPrimary, nrmSecondary, blendWeight));
    nrmSample.xy *= material.surfaceFactors.z; // normal scale
    float3 N = normalize(mul(TBN, nrmSample));

    // ---- PBR factors ------------------------------------------------------
    float metallicSample = lerp(
        MaterialTexture(metallicTexture, MaterialSlotUv(material, input.fragDrawSlot, 2u, input.fragTexCoord, input.fragTexCoord1)).b,
        MaterialTexture(secondaryMetallicTexture, input.fragTexCoord).b,
        blendWeight);
    float roughnessSample = lerp(
        MaterialTexture(roughnessTexture, MaterialSlotUv(material, input.fragDrawSlot, 3u, input.fragTexCoord, input.fragTexCoord1)).g,
        MaterialTexture(secondaryRoughnessTexture, input.fragTexCoord).g,
        blendWeight);
    float aoSample = lerp(
        MaterialTexture(occlusionTexture, MaterialSlotUv(material, input.fragDrawSlot, 4u, input.fragTexCoord, input.fragTexCoord1)).r,
        MaterialTexture(secondaryOcclusionTexture, input.fragTexCoord).r,
        blendWeight);
    float3 emissiveSample = lerp(
        MaterialTexture(emissiveTexture, MaterialSlotUv(material, input.fragDrawSlot, 5u, input.fragTexCoord, input.fragTexCoord1)).rgb,
        MaterialTexture(secondaryEmissiveTexture, input.fragTexCoord).rgb,
        blendWeight);

    float metallic = clamp(material.surfaceFactors.x * metallicSample, 0.0, 1.0);
    float roughness = clamp(material.surfaceFactors.y * roughnessSample, 0.04, 1.0);
    MaterialLayers layers = EvaluateMaterialLayers(material, input.fragDrawSlot, input.fragTexCoord, input.fragTexCoord1, TBN, N);
    // Both variations are taken here, in uniform control flow. The base's lobe varies with the
    // normal-mapped normal; the coat's with its own (the geometric normal unless it has a map).
    roughness = FilterRoughnessForSpecularAA(roughness, NormalVariation(N));
    float coatNormalVariation = NormalVariation(layers.coatNormal);
    float ao = lerp(1.0, aoSample, clamp(material.surfaceFactors.w, 0.0, 1.0));

    // ---- Encode -----------------------------------------------------------
    // Albedo is written linear; the _SRGB format encodes it in hardware and the lighting pass's
    // sampler decodes it. Values above 1.0 would clamp here where the forward path kept them, which
    // glTF's [0, 1] base color factor rules out for imported materials.
    output.outAlbedo = float4(albedo.rgb, 1.0);
    // The geometric normal rides along for the shadow lookup's normal offset (see ShadeSurface).
    // It is already face-flipped, so the lighting pass uses it as decoded.
    output.outNormal = float4(EncodeNormalOctahedral(N), EncodeNormalOctahedral(geoNormal));
#if PATH_TRACE_LAYER_PASS == 2
    // The layer is traced as a plain base: the forward pass multiplies the light back by the full
    // material's lobes, and the shading model's other flags would send the path tracer to images the
    // layer does not have.
    output.outSurface = float4(metallic, roughness, ao, EncodeShadingFlags(layers.flags & SHADING_FLAG_UNLIT));
#else
    output.outSurface = float4(metallic, roughness, ao, EncodeShadingFlags(layers.flags));
#endif
    // Each layer target holds its layer where the flags say so and zeros elsewhere; the lighting
    // pass reads only the flagged ones.
    // The coat's roughness is filtered from the floor the lighting pass would give it; with the
    // filter off it is stored as the material has it, as before.
#if PATH_TRACE_LAYER_PASS == 0
    float coatRoughness = ubo.specularAntiAliasing.x > 0.5
                              ? FilterRoughnessForSpecularAA(clamp(layers.coatRoughness, 0.04, 1.0), coatNormalVariation)
                              : layers.coatRoughness;
    float4 coat = HasShadingFlag(layers.flags, SHADING_FLAG_CLEARCOAT) ? float4(layers.coatFactor, coatRoughness, 0.0, 0.0) : float4(0.0);
    if (HasShadingFlag(layers.flags, SHADING_FLAG_ANISOTROPY))
    {
        // The angle is measured in the frame the lighting pass rebuilds from the normal it decodes,
        // so it is built here from that same normal: N through GB1's half float octahedral encoding.
        float3 storedN = DecodeNormalOctahedral(UnpackHalf2x16(PackHalf2x16(EncodeNormalOctahedral(N))));
        float3 tangent = normalize(layers.anisotropyTangent - storedN * dot(storedN, layers.anisotropyTangent));
        coat.ba = float2(EncodeAnisotropyAngle(storedN, tangent), layers.anisotropyStrength);
    }
    output.outCoat = coat;
    output.outSheen = HasShadingFlag(layers.flags, SHADING_FLAG_SHEEN) ? float4(layers.sheenColor, layers.sheenRoughness) : float4(0.0);
    // The square root spreads the usual dielectric F0 (0.02-0.08) over 36-72 of 255; 0.04 is 51
    // exactly.
    output.outSpecular = HasShadingFlag(layers.flags, SHADING_FLAG_SPECULAR) ? float4(sqrt(layers.dielectricF0), layers.dielectricF90) : float4(0.0);
    // Pre-exposed like the HDR target (see pre_exposure.slang), so an emissive far brighter than
    // B10G11R11's 65000 still fits; the lighting pass divides it back into physical units.
    output.outEmissive = float4(emissiveSample * material.emissiveFactor * ubo.exposure.x, 0.0);
    if (kDecal)
    {
        // The blend factor; the alpha channels themselves are masked off (GB2's holds the flags).
        output.outAlbedo.a = albedo.a;
        output.outSurface.a = albedo.a;
        output.outEmissive.a = albedo.a;
    }
#endif

    // uv = ndc * 0.5 + 0.5 with the Y flip inside the projection, so half the NDC difference is
    // the motion in UV units. A consumer finds the previous position at uv - velocity.
    float2 currNdc = input.fragCurrClip.xy / input.fragCurrClip.w;
    float2 prevNdc = input.fragPrevClip.xy / input.fragPrevClip.w;
    output.outVelocity = float4((currNdc - prevNdc) * 0.5, HasShadingFlag(layers.flags, SHADING_FLAG_COAT_NORMAL) ? EncodeNormalOctahedral(layers.coatNormal) : float2(0.0));
#if PATH_TRACE_LAYER_PASS == 2
    // Only the fragment at the depth the depth pass kept, decided last, after every derivative.
    if ((kBlendItem && albedo.a < PATH_TRACE_LAYER_MIN_ALPHA) || pathTraceLayerDepth.Load(int3(int2(input.fragCoord.xy), 0)).r != input.fragCoord.z)
        discard;
#endif
#endif
    return output;
}
