// Adds the one-bounce indirect diffuse (SceneGi, gi_resolve.comp) to the lit HDR target, blended
// ONE + ONE over what the lighting pass wrote. The light arrives as the ambient term's does, the
// average radiance over the cosine-weighted hemisphere, so it goes through the same diffuse albedo:
// the dielectric share under the dielectric's specular albedo (EvaluateBaseSpecularAlbedos), the
// material's own occlusion (the trace already accounts for the screen-space one), then what the
// sheen and coat layers above the base let through, and the aerial perspective's transmittance.
// Pixels the forward pass shades, unlit ones and the background get nothing.

#include "scene_common.slang"
#include "atmosphere_sampling.slang"
#include "pbr_common.slang"
#include "gbuffer_common.slang"
#include "gbuffer_inputs.slang"

struct FragmentInput
{
    [[vk::location(0)]] float2 fragTexCoord : TEXCOORD0;
};

struct FragmentOutput
{
    [[vk::location(0)]] float4 outColor : SV_Target0;
};

[shader("fragment")]
FragmentOutput main(FragmentInput input)
{
    FragmentOutput output;
    output.outColor = float4(0.0);
    float depth = gbufferDepth.Sample(gbufferSampler, input.fragTexCoord).r;
    if (IsFarDepth(depth))
    {
        return output;
    }
    float4 surface = gbufferSurface.Sample(gbufferSampler, input.fragTexCoord);
    uint flags = DecodeShadingFlags(surface.a);
    if ((flags & (SHADING_FLAG_UNLIT | SHADING_FLAG_FORWARD)) != 0u)
    {
        return output;
    }
    // Pre-exposed, as the HDR target it came from.
    float3 indirect = sceneGi.Sample(gbufferSampler, input.fragTexCoord).rgb;
    if (all(indirect == float3(0.0)))
    {
        return output;
    }

    float3 albedo = gbufferAlbedo.Sample(gbufferSampler, input.fragTexCoord).rgb;
    float3 N = DecodeNormalOctahedral(gbufferNormal.Sample(gbufferSampler, input.fragTexCoord).rg);
    float metallic = surface.r;
    float roughness = clamp(surface.g, 0.04, 1.0);
    float4 world = mul(ubo.invViewProj, float4(input.fragTexCoord * 2.0 - 1.0, depth, 1.0));
    float3 worldPosition = world.xyz / world.w;
    float3 V = normalize(ubo.cameraWorldPosition.xyz - worldPosition);
    float NdV = max(dot(N, V), 0.0);

    SpecularParams specular = NoSpecularOverride();
    if ((flags & SHADING_FLAG_SPECULAR) != 0u)
    {
        float4 specularData = gbufferSpecular.Sample(gbufferSampler, input.fragTexCoord);
        specular.dielectricF0 = specularData.rgb * specularData.rgb;
        specular.dielectricF90 = specularData.a;
    }
    BaseSpecularAlbedos albedos = EvaluateBaseSpecularAlbedos(SampleEnvironmentBrdf(roughness, NdV), albedo, metallic, specular);
    float3 diffuse = albedo * (1.0 - metallic) * (float3(1.0) - albedos.dielectric) * indirect * surface.b;

    if ((flags & SHADING_FLAG_SHEEN) != 0u)
    {
        float4 sheenData = gbufferSheen.Sample(gbufferSampler, input.fragTexCoord);
        float sheenRoughness = clamp(sheenData.a, 0.04, 1.0);
        diffuse *= 1.0 - max(sheenData.r, max(sheenData.g, sheenData.b)) * SampleSheenAlbedo(sheenRoughness, NdV);
    }
    if ((flags & SHADING_FLAG_CLEARCOAT) != 0u)
    {
        float coatFactor = gbufferCoat.Sample(gbufferSampler, input.fragTexCoord).r;
        float3 coatNormal = (flags & SHADING_FLAG_COAT_NORMAL) != 0u
                              ? DecodeNormalOctahedral(gbufferVelocity.Sample(gbufferSampler, input.fragTexCoord).ba)
                              : DecodeNormalOctahedral(gbufferNormal.Sample(gbufferSampler, input.fragTexCoord).ba);
        diffuse *= 1.0 - coatFactor * FresnelSchlick(max(dot(coatNormal, V), 0.0), COAT_F0).x;
    }

    // The aerial perspective dims what lies behind it by its transmittance and adds its own light,
    // which the lighting pass already added once.
    float3 transmittance = ApplyAerialPerspective(float3(1.0), worldPosition) - ApplyAerialPerspective(float3(0.0), worldPosition);
    output.outColor = float4(diffuse * transmittance, 0.0);
    return output;
}
