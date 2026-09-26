#version 450
#extension GL_GOOGLE_include_directive : require

// Adds the one-bounce indirect diffuse (SceneGi, gi_resolve.comp) to the lit HDR target, blended
// ONE + ONE over what the lighting pass wrote. The light arrives as the ambient term's does, the
// average radiance over the cosine-weighted hemisphere, so it goes through the same diffuse albedo:
// the dielectric share under the dielectric's specular albedo (EvaluateBaseSpecularAlbedos), the
// material's own occlusion (the trace already accounts for the screen-space one), then what the
// sheen and coat layers above the base let through, and the aerial perspective's transmittance.
// Pixels the forward pass shades, unlit ones and the background get nothing.

#include "scene_common.glsl"
#include "atmosphere_sampling.glsl"
#include "pbr_common.glsl"
#include "gbuffer_common.glsl"
#include "gbuffer_inputs.glsl"

layout(location = 0) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(0.0);
    float depth = texture(gbufferDepth, fragTexCoord).r;
    if (depth >= 1.0)
    {
        return;
    }
    vec4 surface = texture(gbufferSurface, fragTexCoord);
    uint flags = DecodeShadingFlags(surface.a);
    if ((flags & (SHADING_FLAG_UNLIT | SHADING_FLAG_FORWARD)) != 0u)
    {
        return;
    }
    // Pre-exposed, as the HDR target it came from.
    vec3 indirect = texture(sceneGi, fragTexCoord).rgb;
    if (all(equal(indirect, vec3(0.0))))
    {
        return;
    }

    vec3 albedo = texture(gbufferAlbedo, fragTexCoord).rgb;
    vec3 N = DecodeNormalOctahedral(texture(gbufferNormal, fragTexCoord).rg);
    float metallic = surface.r;
    float roughness = clamp(surface.g, 0.04, 1.0);
    vec4 world = ubo.invViewProj * vec4(fragTexCoord * 2.0 - 1.0, depth, 1.0);
    vec3 worldPosition = world.xyz / world.w;
    vec3 V = normalize(ubo.cameraWorldPosition.xyz - worldPosition);
    float NdV = max(dot(N, V), 0.0);

    SpecularParams specular = NoSpecularOverride();
    if ((flags & SHADING_FLAG_SPECULAR) != 0u)
    {
        vec4 specularData = texture(gbufferSpecular, fragTexCoord);
        specular.dielectricF0 = specularData.rgb * specularData.rgb;
        specular.dielectricF90 = specularData.a;
    }
    BaseSpecularAlbedos albedos = EvaluateBaseSpecularAlbedos(SampleEnvironmentBrdf(roughness, NdV), albedo, metallic, specular);
    vec3 diffuse = albedo * (1.0 - metallic) * (vec3(1.0) - albedos.dielectric) * indirect * surface.b;

    if ((flags & SHADING_FLAG_SHEEN) != 0u)
    {
        vec4 sheenData = texture(gbufferSheen, fragTexCoord);
        float sheenRoughness = clamp(sheenData.a, 0.04, 1.0);
        diffuse *= 1.0 - max(sheenData.r, max(sheenData.g, sheenData.b)) * SampleSheenAlbedo(sheenRoughness, NdV);
    }
    if ((flags & SHADING_FLAG_CLEARCOAT) != 0u)
    {
        float coatFactor = texture(gbufferCoat, fragTexCoord).r;
        vec3 coatNormal = (flags & SHADING_FLAG_COAT_NORMAL) != 0u
                              ? DecodeNormalOctahedral(texture(gbufferVelocity, fragTexCoord).ba)
                              : DecodeNormalOctahedral(texture(gbufferNormal, fragTexCoord).ba);
        diffuse *= 1.0 - coatFactor * FresnelSchlick(max(dot(coatNormal, V), 0.0), COAT_F0).x;
    }

    // The aerial perspective dims what lies behind it by its transmittance and adds its own light,
    // which the lighting pass already added once.
    vec3 transmittance = ApplyAerialPerspective(vec3(1.0), worldPosition) - ApplyAerialPerspective(vec3(0.0), worldPosition);
    outColor = vec4(diffuse * transmittance, 0.0);
}
