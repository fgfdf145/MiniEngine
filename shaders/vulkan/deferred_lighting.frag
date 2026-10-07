#version 460
#extension GL_GOOGLE_include_directive : require
#ifdef RAY_QUERY
// The ray query variant (deferred_lighting_ray_query.frag.spv) traces the local lights' shadows.
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_EXT_nonuniform_qualifier : require
#define LOCAL_SHADOW_RAYS
#define RAY_TEXTURED_ALPHA
#endif

#include "scene_common.glsl"
#include "atmosphere_sampling.glsl"
#include "pbr_common.glsl"
#include "gbuffer_common.glsl"
#include "gbuffer_inputs.glsl"
#include "pre_exposure.glsl"
#ifdef RAY_QUERY
// The ray scene at set 1, which the plain variant leaves empty, and its texture table at set 3.
#include "ray_tracing_common.glsl"
#include "material_common.glsl"
#include "material_uv.glsl"
#include "ray_hit_common.glsl"
#endif

// Must match the push constant VulkanLightingPass::Record pushes.
layout(push_constant) uniform LightingConstants
{
    // xyz = kViewportBackgroundFrameBuffer, exactly what the forward pass clears the HDR target to;
    // w unused. A pixel no geometry covered resolves to it.
    vec4 backgroundRadiance;
    // x = 1 for the light cluster heat map instead of shading; y = 1 to take the sun's shadow from the
    // ray traced one (SceneShadow) instead of the cascades; z = 1 to trace the local lights' shadows
    // (the ray query variant only); w = 1 for path tracing mode: the path traced light (SceneGi and
    // SceneReflections, path_trace_pass.h) in place of every ambient term; w = 2 for ReSTIR PT: its light
    // (scenePathTrace, restir_pt_pass.h) in place of all the shading.
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

#ifdef RAY_QUERY
float LocalShadowNoise(vec2 pixel)
{
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// One ray from the surface toward a point on the light's source: its sphere (point and spot lights
// with a source radius) or its rectangle (area lights), drawn afresh each frame so TAA averages a
// soft shadow; a light without a size casts a hard one. The ray stops short of the light by the
// atlas's near plane, so a lamp's own housing does not shadow it.
float TraceLocalLightShadow(SceneLightData light, vec3 worldPosition, vec3 offsetNormal)
{
    float frame = ubo.cloudParams.w;
    vec2 u = vec2(LocalShadowNoise(gl_FragCoord.xy + 5.588238 * frame),
                  LocalShadowNoise(gl_FragCoord.yx + 7.123 * frame + light.positionAndRange.xy));
    vec3 target = light.positionAndRange.xyz;
    if (int(light.directionAndType.w) == LIGHT_AREA)
    {
        vec3 lightNormal = normalize(light.directionAndType.xyz);
        vec3 rightAxis = normalize(light.areaRightAxis.xyz);
        vec3 upAxis = cross(lightNormal, rightAxis);
        target += rightAxis * ((u.x - 0.5) * light.spotAndArea.z) + upAxis * ((u.y - 0.5) * light.spotAndArea.w);
    }
    else if (light.spotAndArea.z > 0.0)
    {
        // A disk of the source's radius facing the surface: the sphere as the surface sees it.
        vec3 axis = normalize(worldPosition - target);
        vec3 helper = abs(axis.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        vec3 tangent = normalize(cross(helper, axis));
        vec3 bitangent = cross(axis, tangent);
        float radius = light.spotAndArea.z * sqrt(u.x);
        float phi = 6.28318530718 * u.y;
        target += (tangent * cos(phi) + bitangent * sin(phi)) * radius;
    }
    float distanceToCamera = length(worldPosition - ubo.cameraWorldPosition.xyz);
    vec3 origin = OffsetRayOrigin(worldPosition + offsetNormal * (0.002 + 0.0004 * distanceToCamera), offsetNormal);
    vec3 toTarget = target - origin;
    float distance = length(toTarget);
    float reach = distance - kLocalShadowNearPlaneMetres;
    if (reach <= 0.0)
    {
        return 1.0;
    }
    RayHit hit;
    uint rayId = uint(gl_FragCoord.x) * 73856093u ^ uint(gl_FragCoord.y) * 19349663u ^ uint(frame) * 83492791u;
    return TraceSceneRayMasked(origin, toTarget / distance, 0.0, reach, true, rayId, RAY_MASK_VISIBILITY, hit) ? 0.0 : 1.0;
}
#endif

void main()
{
    float depth = texture(gbufferDepth, fragTexCoord).r;
    if (IsFarDepth(depth))
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
    // Material occlusion times the screen-space (or ray traced) result. Only the ambient term uses it;
    // the resolve writes 1.0 when AO is off. g is the ray traced DDGI probe occlusion, 1 without it.
    vec2 aoSample = texture(sceneAo, fragTexCoord).rg;
    float ao = surface.b * aoSample.r;
    ddgiProbeOcclusion = aoSample.g;
    if (lightingData.debug.y > 0.5)
    {
        tracedSunShadow = texture(sceneShadow, fragTexCoord).r;
    }
#ifdef RAY_QUERY
    tracedLocalShadows = lightingData.debug.z > 0.5;
#endif
    // GB3 is pre-exposed; shading runs in physical units, so it is divided back here.
    vec3 emissive = texture(gbufferEmissive, fragTexCoord).rgb * ubo.exposure.y;

    // fragTexCoord has its origin at the top left, and the image's top row is ndc.y == -1 (see
    // fullscreen.vert). The projection's Y flip is inside invViewProj, so no flip belongs here.
    // Depth is 0..1 (reverse-Z, reverse_depth.glsl); invViewProj undoes it whichever way it runs.
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
    uint flags = DecodeShadingFlags(surface.a);
    // Unlit: the base colour as if lit to the display's paper white, at every exposure, like the
    // viewport background.
    if ((flags & SHADING_FLAG_UNLIT) != 0u)
    {
        outColor = vec4(albedo * kFrameBufferUnitsPerExposed, 1.0);
        return;
    }
    // The forward pass shades this pixel over whatever is written here.
    if ((flags & SHADING_FLAG_FORWARD) != 0u)
    {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    if (lightingData.debug.w > 1.5)
    {
        // ReSTIR PT (restir_pt_spatial.comp) carries all the light the surface reflects, direct and
        // indirect; only the surface's own emission is added here. Its debug views (a = 1) show as they are.
        vec4 traced = texture(scenePathTrace, fragTexCoord);
        if (traced.a > 0.5)
        {
            outColor = vec4(traced.rgb, 1.0);
            return;
        }
        vec3 tracedColor = emissive + traced.rgb * ubo.exposure.y;
        outColor = vec4(ApplyAerialPerspective(tracedColor, worldPosition) * ubo.exposure.x, 1.0);
        return;
    }
    CoatParams coat = NoCoat();
    SheenParams sheen = NoSheen();
    AnisotropyParams anisotropy = NoAnisotropy();
    SpecularParams specular = NoSpecularOverride();
    if ((flags & (SHADING_FLAG_CLEARCOAT | SHADING_FLAG_ANISOTROPY)) != 0u)
    {
        vec4 coatData = texture(gbufferCoat, fragTexCoord);
        if ((flags & SHADING_FLAG_CLEARCOAT) != 0u)
        {
            coat.factor = coatData.r;
            coat.roughness = clamp(coatData.g, 0.04, 1.0);
            coat.normal = (flags & SHADING_FLAG_COAT_NORMAL) != 0u
                              ? DecodeNormalOctahedral(texture(gbufferVelocity, fragTexCoord).ba)
                              : geoNormal;
        }
        if ((flags & SHADING_FLAG_ANISOTROPY) != 0u)
        {
            anisotropy.tangent = DecodeAnisotropyTangent(N, coatData.b);
            anisotropy.strength = coatData.a;
        }
    }
    if ((flags & SHADING_FLAG_SHEEN) != 0u)
    {
        vec4 sheenData = texture(gbufferSheen, fragTexCoord);
        sheen.color = sheenData.rgb;
        sheen.roughness = clamp(sheenData.a, 0.04, 1.0);
    }
    if ((flags & SHADING_FLAG_SPECULAR) != 0u)
    {
        vec4 specularData = texture(gbufferSpecular, fragTexCoord);
        specular.dielectricF0 = specularData.rgb * specularData.rgb;
        specular.dielectricF90 = specularData.a;
    }
    // Screen-space reflection in HDR target units; ShadeSurface wants physical radiance.
    vec4 reflection = texture(sceneReflections, fragTexCoord);
    reflection.rgb *= ubo.exposure.y;
    if (lightingData.debug.w > 0.5)
    {
        // Path tracing mode: the two targets hold the demodulated traced light, pre-exposed.
        pathTracedIndirect = true;
        pathTracedDiffuse = texture(sceneGi, fragTexCoord).rgb * ubo.exposure.y;
        pathTracedSpecular = reflection.rgb;
        reflection = vec4(0.0);
    }
    vec3 color = ShadeSurface(worldPosition, N, geoNormal, V, albedo, metallic, roughness, ao, emissive, coat, sheen, anisotropy, specular, reflection);

    // Opaque and Mask fragments are fully covered by definition; the forward blend pass
    // composites over this with an RGB-only write mask. Pre-exposed on the way out (see
    // pre_exposure.glsl), after the aerial perspective, which is physical radiance too.
    outColor = vec4(ApplyAerialPerspective(color, worldPosition) * ubo.exposure.x, 1.0);
}
