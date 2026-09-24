#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"
#include "atmosphere_sampling.glsl"
#include "pbr_common.glsl"
#include "gbuffer_common.glsl"
#include "gbuffer_inputs.glsl"
#include "pre_exposure.glsl"

// Must match the push constant VulkanLightingPass::Record pushes.
layout(push_constant) uniform LightingConstants
{
    // xyz = kViewportBackgroundFrameBuffer, exactly what the forward pass clears the HDR target to;
    // w unused. A pixel no geometry covered resolves to it.
    vec4 backgroundRadiance;
    // x = 1 for the light cluster heat map instead of shading; yzw unused.
    vec4 debug;
}
lightingData;

// Blue through cyan, green and yellow to red as the count rises to 32, black for no light at all.
vec3 LightCountHeat(uint count)
{
    if (count == 0u)
    {
        return vec3(0.0);
    }
    float t = clamp(float(count) / 32.0, 0.0, 1.0);
    return clamp(vec3(1.5) - abs(vec3(4.0 * t) - vec3(3.0, 2.0, 1.0)), 0.0, 1.0);
}

layout(location = 0) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

void main()
{
    float depth = texture(gbufferDepth, fragTexCoord).r;
    if (depth >= 1.0)
    {
        outColor = vec4(lightingData.backgroundRadiance.rgb, 1.0);
        return;
    }

    vec3 albedo = texture(gbufferAlbedo, fragTexCoord).rgb;
    vec4 normals = texture(gbufferNormal, fragTexCoord);
    vec3 N = DecodeNormalOctahedral(normals.rg);
    // Already face-flipped by gbuffer.frag; the shadow lookup offsets along it.
    vec3 geoNormal = DecodeNormalOctahedral(normals.ba);
    vec4 surface = texture(gbufferSurface, fragTexCoord);
    float metallic = surface.r;
    // Re-clamped: 8-bit storage can round the geometry pass's 0.04 floor down to 10/255, and the
    // GGX terms assume the floor holds.
    float roughness = clamp(surface.g, 0.04, 1.0);
    // Material occlusion times the screen-space result. Only the ambient term uses it; the resolve
    // writes 1.0 when AO is off.
    float ao = surface.b * texture(sceneAo, fragTexCoord).r;
    // GB3 is pre-exposed; shading runs in physical units, so it is divided back here.
    vec3 emissive = texture(gbufferEmissive, fragTexCoord).rgb * ubo.exposure.y;

    // fragTexCoord has its origin at the top left, and the image's top row is ndc.y == -1 (see
    // fullscreen.vert). The projection's Y flip is inside invViewProj, so no flip belongs here.
    // Depth is 0..1 because the projection is perspectiveRH_ZO.
    vec4 world = ubo.invViewProj * vec4(fragTexCoord * 2.0 - 1.0, depth, 1.0);
    vec3 worldPosition = world.xyz / world.w;

    if (lightingData.debug.x > 0.5)
    {
        uint localLights = ubo.lightCounts.z != 0u
                               ? lightClusters.ranges[FindLightCluster(worldPosition)].y
                               : ubo.lightCounts.y - ubo.lightCounts.x;
        // Scaled so the tone mapping pass, which shows this view at kExposedPerFrameBufferUnit,
        // displays the heat colours as written.
        outColor = vec4(LightCountHeat(localLights) * kFrameBufferUnitsPerExposed, 1.0);
        return;
    }

    vec3 V = normalize(ubo.cameraWorldPosition.xyz - worldPosition);
    // GB5 means something only for the models that write it, so it is read only for them.
    uint shadingModel = DecodeShadingModel(surface.a);
    CoatParams coat = NoCoat();
    SheenParams sheen = NoSheen();
    if (shadingModel == SHADING_MODEL_CLEARCOAT)
    {
        vec2 coatFactors = texture(gbufferCustom, fragTexCoord).rg;
        coat.factor = coatFactors.x;
        coat.roughness = clamp(coatFactors.y, 0.04, 1.0);
        coat.normal = geoNormal;
    }
    else if (shadingModel == SHADING_MODEL_SHEEN)
    {
        vec4 sheenFactors = texture(gbufferCustom, fragTexCoord);
        sheen.color = sheenFactors.rgb;
        sheen.roughness = clamp(sheenFactors.a, 0.04, 1.0);
    }
    vec3 color = ShadeSurface(worldPosition, N, geoNormal, V, albedo, metallic, roughness, ao, emissive, coat, sheen);

    // Opaque and Mask fragments are fully covered by definition; the forward blend pass
    // composites over this with an RGB-only write mask. Pre-exposed on the way out (see
    // pre_exposure.glsl), after the aerial perspective, which is physical radiance too.
    outColor = vec4(ApplyAerialPerspective(color, worldPosition) * ubo.exposure.x, 1.0);
}
