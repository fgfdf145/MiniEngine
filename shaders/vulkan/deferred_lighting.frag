#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"
#include "pbr_common.glsl"
#include "gbuffer_common.glsl"
#include "gbuffer_inputs.glsl"

// Must match the push constant VulkanLightingPass::Record pushes.
layout(push_constant) uniform LightingConstants
{
    // xyz = GetBackgroundRadiance(exposure), exactly what the forward pass clears the HDR target
    // to, computed by the same C++ helper; w unused. A pixel no geometry covered resolves to it.
    vec4 backgroundRadiance;
}
lightingData;

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
    vec3 emissive = texture(gbufferEmissive, fragTexCoord).rgb;

    // fragTexCoord has its origin at the top left, and the image's top row is ndc.y == -1 (see
    // fullscreen.vert). The projection's Y flip is inside invViewProj, so no flip belongs here.
    // Depth is 0..1 because the projection is perspectiveRH_ZO.
    vec4 world = ubo.invViewProj * vec4(fragTexCoord * 2.0 - 1.0, depth, 1.0);
    vec3 worldPosition = world.xyz / world.w;

    vec3 V = normalize(ubo.cameraWorldPosition.xyz - worldPosition);
    vec3 color = ShadeSurface(worldPosition, N, geoNormal, V, albedo, metallic, roughness, ao) + emissive;

    // Opaque and Mask fragments are fully covered by definition; the forward blend pass
    // composites over this with an RGB-only write mask.
    outColor = vec4(color, 1.0);
}
