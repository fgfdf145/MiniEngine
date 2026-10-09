#ifdef RAY_QUERY
// The ray query variant (deferred_lighting_ray_query.frag.spv) traces the local lights' shadows.
#define LOCAL_SHADOW_RAYS
#define RAY_TEXTURED_ALPHA
#endif

#include "scene_common.slang"
#include "atmosphere_sampling.slang"
#include "pbr_common.slang"
#include "gbuffer_common.slang"
#include "gbuffer_inputs.slang"
#include "pre_exposure.slang"
#ifdef RAY_QUERY
// The ray scene at set 1, which the plain variant leaves empty, and its texture table at set 3.
#include "ray_tracing_common.slang"
#include "material_common.slang"
#include "material_uv.slang"
#include "ray_hit_common.slang"
#endif

// Must match the push constant VulkanLightingPass::Record pushes.
struct LightingConstants
{
    // xyz = kViewportBackgroundFrameBuffer, exactly what the forward pass clears the HDR target to;
    // w unused. A pixel no geometry covered resolves to it.
    float4 backgroundRadiance;
    // x = 1 for the light cluster heat map instead of shading; y = 1 to take the sun's shadow from the
    // ray traced one (SceneShadow) instead of the cascades; z = 1 to trace the local lights' shadows
    // (the ray query variant only); w = 1 for path tracing mode: the path traced light (SceneGi and
    // SceneReflections, path_trace_pass.h) in place of every ambient term; w = 2 for ReSTIR PT: its light
    // (scenePathTrace, restir_pt_pass.h) in place of all the shading; w = 3 for the path tracer's offline
    // mode: the two targets in place of the ambient terms and the direct lights.
    float4 debug;
};
[[vk::push_constant]] ConstantBuffer<LightingConstants> lightingData;

// Blue through cyan, green and yellow to red as the count rises to 32, black for no light at all.
float3 LightCountHeat(uint count)
{
    if (count == 0u)
    {
        return float3(0.0);
    }
    float t = clamp(float(count) / 32.0, 0.0, 1.0);
    return clamp(float3(1.5) - abs(float3(4.0 * t) - float3(3.0, 2.0, 1.0)), 0.0, 1.0);
}

// The pixel being shaded (SV_Position.xy), for the traced local shadows' noise:
// TraceLocalLightShadow is called from pbr_common.slang's light loop, which does not pass it.
static float2 shadedPixel;

#ifdef RAY_QUERY
float LocalShadowNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// One ray from the surface toward a point on the light's source: its sphere (point and spot lights
// with a source radius) or its rectangle (area lights), drawn afresh each frame so TAA averages a
// soft shadow; a light without a size casts a hard one. The ray stops short of the light by the
// atlas's near plane, so a lamp's own housing does not shadow it.
float TraceLocalLightShadow(SceneLightData light, float3 worldPosition, float3 offsetNormal)
{
    float frame = ubo.cloudParams.w;
    float2 u = float2(LocalShadowNoise(shadedPixel + 5.588238 * frame),
                      LocalShadowNoise(shadedPixel.yx + 7.123 * frame + light.positionAndRange.xy));
    float3 target = light.positionAndRange.xyz;
    if (int(light.directionAndType.w) == LIGHT_AREA)
    {
        float3 lightNormal = normalize(light.directionAndType.xyz);
        float3 rightAxis = normalize(light.areaRightAxis.xyz);
        float3 upAxis = cross(lightNormal, rightAxis);
        target += rightAxis * ((u.x - 0.5) * light.spotAndArea.z) + upAxis * ((u.y - 0.5) * light.spotAndArea.w);
    }
    else if (light.spotAndArea.z > 0.0)
    {
        // A disk of the source's radius facing the surface: the sphere as the surface sees it.
        float3 axis = normalize(worldPosition - target);
        float3 helper = abs(axis.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
        float3 tangent = normalize(cross(helper, axis));
        float3 bitangent = cross(axis, tangent);
        float radius = light.spotAndArea.z * sqrt(u.x);
        float phi = 6.28318530718 * u.y;
        target += (tangent * cos(phi) + bitangent * sin(phi)) * radius;
    }
    float distanceToCamera = length(worldPosition - ubo.cameraWorldPosition.xyz);
    float3 origin = OffsetRayOrigin(worldPosition + offsetNormal * (0.002 + 0.0004 * distanceToCamera), offsetNormal);
    float3 toTarget = target - origin;
    float distance = length(toTarget);
    float reach = distance - kLocalShadowNearPlaneMetres;
    if (reach <= 0.0)
    {
        return 1.0;
    }
    RayHit hit;
    uint rayId = uint(shadedPixel.x) * 73856093u ^ uint(shadedPixel.y) * 19349663u ^ uint(frame) * 83492791u;
    return TraceSceneRayMasked(origin, toTarget / distance, 0.0, reach, true, rayId, RAY_MASK_SHADOW, hit) ? 0.0 : 1.0;
}
#endif

struct FragmentInput
{
    [[vk::location(0)]] float2 fragTexCoord;
    float4 fragCoord : SV_Position;
};

struct FragmentOutput
{
    [[vk::location(0)]] float4 outColor : SV_Target0;
};

[shader("fragment")]
FragmentOutput main(FragmentInput input)
{
    FragmentOutput output;
    shadedPixel = input.fragCoord.xy;
    float depth = gbufferDepth.Sample(gbufferSampler, input.fragTexCoord).r;
    if (IsFarDepth(depth))
    {
        output.outColor = float4(lightingData.backgroundRadiance.rgb, 1.0);
        return output;
    }

    float3 albedo = gbufferAlbedo.Sample(gbufferSampler, input.fragTexCoord).rgb;
    float4 normals = gbufferNormal.Sample(gbufferSampler, input.fragTexCoord);
    float3 N = DecodeNormalOctahedral(normals.rg);
    // Already face-flipped by gbuffer.frag; the shadow lookup offsets along it.
    float3 geoNormal = DecodeNormalOctahedral(normals.ba);
    float4 surface = gbufferSurface.Sample(gbufferSampler, input.fragTexCoord);
    float metallic = surface.r;
    // Re-clamped: 8-bit storage can round the geometry pass's 0.04 floor down to 10/255, and the
    // GGX terms assume the floor holds.
    float roughness = clamp(surface.g, 0.04, 1.0);
    // Material occlusion times the screen-space (or ray traced) result. Only the ambient term uses it;
    // the resolve writes 1.0 when AO is off. g is the ray traced DDGI probe occlusion, 1 without it.
    float2 aoSample = sceneAo.Sample(gbufferSampler, input.fragTexCoord).rg;
    float ao = surface.b * aoSample.r;
    ddgiProbeOcclusion = aoSample.g;
    if (lightingData.debug.y > 0.5)
    {
        tracedSunShadow = sceneShadow.Sample(gbufferSampler, input.fragTexCoord).r;
    }
#ifdef RAY_QUERY
    tracedLocalShadows = lightingData.debug.z > 0.5;
#endif
    // GB3 is pre-exposed; shading runs in physical units, so it is divided back here.
    float3 emissive = gbufferEmissive.Sample(gbufferSampler, input.fragTexCoord).rgb * ubo.exposure.y;

    // input.fragTexCoord has its origin at the top left, and the image's top row is ndc.y == -1 (see
    // fullscreen.vert). The projection's Y flip is inside invViewProj, so no flip belongs here.
    // Depth is 0..1 (reverse-Z, reverse_depth.slang); invViewProj undoes it whichever way it runs.
    float4 world = mul(ubo.invViewProj, float4(input.fragTexCoord * 2.0 - 1.0, depth, 1.0));
    float3 worldPosition = world.xyz / world.w;

    if (lightingData.debug.x > 0.5)
    {
        uint localLights = ubo.lightCounts.z != 0u
                               ? LightClusterRange(FindLightCluster(worldPosition)).y
                               : ubo.lightCounts.y - ubo.lightCounts.x;
        // Scaled so the tone mapping pass, which shows this view at kExposedPerFrameBufferUnit,
        // displays the heat colours as written.
        output.outColor = float4(LightCountHeat(localLights) * kFrameBufferUnitsPerExposed, 1.0);
        return output;
    }

    float3 V = normalize(ubo.cameraWorldPosition.xyz - worldPosition);
    // GB5 means something only for the models that write it, so it is read only for them.
    uint flags = DecodeShadingFlags(surface.a);
    // Unlit: the base colour as if lit to the display's paper white, at every exposure, like the
    // viewport background.
    if ((flags & SHADING_FLAG_UNLIT) != 0u)
    {
        output.outColor = float4(albedo * kFrameBufferUnitsPerExposed, 1.0);
        return output;
    }
    // The forward pass shades this pixel over whatever is written here.
    if ((flags & SHADING_FLAG_FORWARD) != 0u)
    {
        output.outColor = float4(0.0, 0.0, 0.0, 1.0);
        return output;
    }
    if (lightingData.debug.w > 1.5 && lightingData.debug.w < 2.5)
    {
        // ReSTIR PT (restir_pt_spatial.comp) carries all the light the surface reflects, direct and
        // indirect; only the surface's own emission is added here. Its debug views (a = 1) show as they are.
        float4 traced = scenePathTrace.Sample(gbufferSampler, input.fragTexCoord);
        if (traced.a > 0.5)
        {
            output.outColor = float4(traced.rgb, 1.0);
            return output;
        }
        float3 tracedColor = emissive + traced.rgb * ubo.exposure.y;
        output.outColor = float4(ApplyAerialPerspective(tracedColor, worldPosition) * ubo.exposure.x, 1.0);
        return output;
    }
    CoatParams coat = NoCoat();
    SheenParams sheen = NoSheen();
    AnisotropyParams anisotropy = NoAnisotropy();
    SpecularParams specular = NoSpecularOverride();
    if ((flags & (SHADING_FLAG_CLEARCOAT | SHADING_FLAG_ANISOTROPY)) != 0u)
    {
        float4 coatData = gbufferCoat.Sample(gbufferSampler, input.fragTexCoord);
        if ((flags & SHADING_FLAG_CLEARCOAT) != 0u)
        {
            coat.factor = coatData.r;
            coat.roughness = clamp(coatData.g, 0.04, 1.0);
            coat.normal = (flags & SHADING_FLAG_COAT_NORMAL) != 0u
                              ? DecodeNormalOctahedral(gbufferVelocity.Sample(gbufferSampler, input.fragTexCoord).ba)
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
        float4 sheenData = gbufferSheen.Sample(gbufferSampler, input.fragTexCoord);
        sheen.color = sheenData.rgb;
        sheen.roughness = clamp(sheenData.a, 0.04, 1.0);
    }
    if ((flags & SHADING_FLAG_SPECULAR) != 0u)
    {
        float4 specularData = gbufferSpecular.Sample(gbufferSampler, input.fragTexCoord);
        specular.dielectricF0 = specularData.rgb * specularData.rgb;
        specular.dielectricF90 = specularData.a;
    }
    // Screen-space reflection in HDR target units; ShadeSurface wants physical radiance.
    float4 reflection = sceneReflections.Sample(gbufferSampler, input.fragTexCoord);
    reflection.rgb *= ubo.exposure.y;
    if (lightingData.debug.w > 0.5)
    {
        // Path tracing mode: the two targets hold the demodulated traced light, pre-exposed.
        pathTracedIndirect = true;
        pathTracedDirect = lightingData.debug.w > 2.5;
        pathTracedDiffuse = sceneGi.Sample(gbufferSampler, input.fragTexCoord).rgb * ubo.exposure.y;
        pathTracedSpecular = reflection.rgb;
        reflection = float4(0.0);
    }
    float3 color = ShadeSurface(worldPosition, N, geoNormal, V, albedo, metallic, roughness, ao, emissive, coat, sheen, anisotropy, specular, reflection);

    // Opaque and Mask fragments are fully covered by definition; the forward blend pass
    // composites over this with an RGB-only write mask. Pre-exposed on the way out (see
    // pre_exposure.slang), after the aerial perspective, which is physical radiance too.
    output.outColor = float4(ApplyAerialPerspective(color, worldPosition) * ubo.exposure.x, 1.0);
    return output;
}
