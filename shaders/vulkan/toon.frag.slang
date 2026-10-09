#include "specialization.slang"

// The toon pass (toon_pass.h): an anime character's cel shading as AnimateApp's "Anime Forward"
// pass computes it, or under kOutline its outline's colour ("Anime Outline"). See
// docs/design/2026-10-07-yuki-toon-shading-design.md for the shader this follows.
//
// The shading is in the shader's own units, where the main light is about 1 (Unity's directional
// light at intensity 1, its colour ToonLight.color) and the result is the albedo where lit; the
// light's luminance then takes it into the scene's radiance, the pre-exposure into the HDR target.
SPECIALIZATION_BOOL(0, kOutline);

#include "shader_helpers.slang"
#include "scene_common.slang"
#include "atmosphere_sampling.slang"
#include "pbr_common.slang"
#include "toon_common.slang"

// The toon prepass's targets (VulkanToonPass's set 3): the characters' linear view depth and the eye
// mask, fetched by pixel.
[[vk::binding(0, 3)]] Texture2D toonLinearDepth : register(t0, space3);
[[vk::binding(1, 3)]] Texture2D toonMask : register(t1, space3);

struct FragmentInput
{
    [[vk::location(0)]] float3 fragWorldPosition : TEXCOORD0;
    [[vk::location(1)]] float3 fragWorldNormal : TEXCOORD1;
    [[vk::location(2)]] float2 fragTexCoord : TEXCOORD2;
    [[vk::location(3)]] float2 fragTexCoord1 : TEXCOORD3;
    [[vk::location(4)]] float3 fragSmoothNormal : TEXCOORD4;
    [[vk::location(5)]] float3 fragGeometryNormal : TEXCOORD5;
    [[vk::location(6)]] nointerpolation uint fragToonIndex : TEXCOORD6;
    [[vk::location(7)]] float4 fragCurrClip : TEXCOORD7;
    [[vk::location(8)]] float4 fragPrevClip : TEXCOORD8;
    float4 fragCoord : SV_Position;
    bool frontFacing : SV_IsFrontFace;
};

struct FragmentOutput
{
    [[vk::location(0)]] float4 outColor : SV_Target0;
    // GBufferVelocity, as gbuffer.frag writes it: rg the motion in UV units (current - previous); b
    // the coat normal's x, none here. Blended with the colour's alpha on a transparent surface.
    [[vk::location(1)]] float4 outVelocity : SV_Target1;
};

float4 ToonVelocity(FragmentInput input, float alpha)
{
    float2 current = input.fragCurrClip.xy / input.fragCurrClip.w;
    float2 previous = input.fragPrevClip.xy / input.fragPrevClip.w;
    return float4((current - previous) * 0.5, 0.0, alpha);
}

// The main light as the shader sees it: where it comes from, its colour normalised to unit luminance
// (a white sun is 1, as Unity's directional light at intensity 1), and the luminance the toon
// colours are scaled by into the scene's radiance: what a white diffuse surface facing it would send
// (lux / pi). The shadow casting directional light, else the first, else the ambient light from the
// camera's side.
struct ToonLight
{
    float3 direction;
    float3 color;
    float luminance;
    bool castsShadow;
};

ToonLight ToonMainLight(float3 toCamera)
{
    ToonLight light;
    int index = int(ubo.shadowParams.x);
    light.castsShadow = index >= 0;
    if (index < 0 && ubo.lightCounts.x > 0u)
    {
        index = 0;
    }
    float3 radiance;
    if (index >= 0)
    {
        SceneLightData data = sceneLights[index];
        light.direction = normalize(-data.directionAndType.xyz);
        radiance = data.colorAndIntensity.rgb * data.colorAndIntensity.w / PI;
    }
    else
    {
        light.direction = toCamera;
        radiance = ubo.ambientLuminance.rgb;
    }
    light.luminance = max(dot(radiance, float3(0.2126, 0.7152, 0.0722)), 0.0);
    light.color = light.luminance > 0.0 ? radiance / light.luminance : float3(0.0);
    return light;
}

// How far toward the sun the world's shadow is looked up from a character's surface: past the near
// side of its own body, so the shadow map does not shade it with itself (NiloToon's receive-shadow
// depth bias; its own shading comes from the cel ramp and the self shadow instead), but still under
// the roof of the car or the building it is in.
static const float kToonWorldShadowLift = 0.2;

// The head (the face, and the hair round it out to the first distance from the head's middle, fading
// out by the second) takes the world's shadow as one, from a point this far above the head bone (about
// the middle of the head) lifted this much further, past the head: a shadow's edge never crosses the
// face or leaves a dark lock over it.
static const float kToonHeadCentreHeight = 0.1;
static const float kToonHeadRadius = 0.15;
static const float2 kToonHeadReach = float2(0.15, 0.3);

// Puts the character in the world's light (the shader's has only the main light, a character in shade
// would glow as if in the sun): the share of the sun that reaches it past everything else (the
// cascades and the clouds), and its light, which that share of the sun turns from the ambient light
// the scene's diffuse surfaces receive there (the DDGI probes, else the sky) toward the sun's. The
// sun's share is returned for the cel shading and the rim light. Unchanged in full sun.
float ToonApplyWorldLight(
    inout ToonLight light, ToonMaterial material, float faceMask, float3 worldPosition, float3 ambientNormal, float3 toCamera)
{
    if (!light.castsShadow)
    {
        return 1.0;
    }
    float3 headCentre = ToonHeadPosition(material) + ToonFaceUp(material) * kToonHeadCentreHeight;
    float head = max(faceMask, 1.0 - smoothstep(kToonHeadReach.x, kToonHeadReach.y, distance(worldPosition, headCentre)));
    float3 shadowPoint = lerp(worldPosition, headCentre, head) + light.direction * (kToonWorldShadowLift + kToonHeadRadius * head);
    float sun = EvaluateDirectionalShadow(shadowPoint, light.direction);
    float3 ambient = SceneDiffuseAmbient(worldPosition, ambientNormal, toCamera);
    float3 sunRadiance = light.color * light.luminance;
    float3 radiance = lerp(ambient, max(sunRadiance, ambient), sun);
    light.luminance = max(dot(radiance, float3(0.2126, 0.7152, 0.0722)), 0.0);
    light.color = light.luminance > 0.0 ? radiance / light.luminance : float3(0.0);
    return sun;
}

float ToonDepthAt(float2 pixel)
{
    int2 size = TextureSize(toonLinearDepth, 0);
    return toonLinearDepth.Load(int3(clamp(int2(pixel), int2(0), size - 1), 0)).r;
}

// Which way a world direction points on screen from this fragment, in pixels (unit length), however
// the projection is flipped; zero where it points at the camera.
float2 ScreenDirection(float3 worldPosition, float3 direction)
{
    float4 a = mul(mul(ubo.proj, ubo.view), float4(worldPosition, 1.0));
    float4 b = mul(mul(ubo.proj, ubo.view), float4(worldPosition + direction * 0.01, 1.0));
    float2 size = float2(TextureSize(toonLinearDepth, 0));
    float2 delta = (b.xy / b.w - a.xy / a.w) * size;
    float length2 = dot(delta, delta);
    return length2 > 1e-12 ? delta * rsqrt(length2) : float2(0.0);
}

[shader("fragment")]
FragmentOutput main(FragmentInput input)
{
    FragmentOutput output;
    ToonMaterial material = toonMaterials[input.fragToonIndex];
    LoadMaterialSamplers(material.samplerIndices);

    // The stencil the shader tests, from the eye mask: the front hair stays off the eyes, its redraw
    // keeps to them.
    float eyeMask = toonMask.Load(int3(int2(input.fragCoord.xy), 0)).r;
    if ((ToonHas(material, TOON_FEATURE_STENCIL_NOT_EQUAL) && eyeMask > 0.5) ||
        (ToonHas(material, TOON_FEATURE_STENCIL_EQUAL) && eyeMask < 0.5))
    {
        discard;
    }

    float3 toCamera = normalize(ubo.cameraWorldPosition.xyz - input.fragWorldPosition);
    float4 base = toonBaseMap.SampleBias(toonBaseMapSampler, input.fragTexCoord, MATERIAL_MIP_BIAS);
    float faceMask = ToonFaceMask(material, input.fragTexCoord);
    float skinMask = ToonSkinMask(material, input.fragTexCoord);
    ToonLight light = ToonMainLight(toCamera);

    if (kOutline)
    {
        // The hull's back faces carry the surface's normal as it is: no face flip, no face fix.
        float3 N = normalize(input.fragGeometryNormal);
        if (ToonHas(material, TOON_FEATURE_ALPHA_TEST) && base.a * material.baseColor.a < material.mainLight.w)
        {
            discard;
        }
        float3 albedo = ToonAlbedo(material, base.rgb, input.fragTexCoord, N, toCamera);
        float sun = ToonApplyWorldLight(light, material, faceMask, input.fragWorldPosition, N, toCamera);
        float lit = clamp(ToonCelShade(material, dot(N, light.direction), input.fragTexCoord) * sun, 0.0, 1.0);
        float3 color = lerp(ToonShadowColor(material, albedo, faceMask, skinMask), albedo, lit) * light.color;
        color *= lerp(material.outlineTint.rgb, material.outlineSkinOverride.rgb, material.outlineSkinOverride.a * skinMask);
        color = ApplyAerialPerspective(color * light.luminance, input.fragWorldPosition) * ubo.exposure.x * drawData.exposureScale;
        output.outColor = float4(color, 1.0);
        output.outVelocity = ToonVelocity(input, 1.0);
        return output;
    }

    float faceSign = input.frontFacing ? 1.0 : -1.0;
    float3 N = normalize(input.fragWorldNormal) * faceSign;
    float3 L = light.direction;
    float NdotL = dot(N, L);
    float NdotV = clamp(dot(N, toCamera), 0.0, 1.0);

    float3 albedo = ToonAlbedo(material, base.rgb, input.fragTexCoord, N, toCamera);
    float3 additive = ToonAdditiveMatCap(material, albedo, input.fragTexCoord, N, toCamera);
    albedo += additive;
    float alpha = ToonAlpha(
        material, base.a * material.baseColor.a, input.fragTexCoord, toCamera, dot(additive, float3(0.212673, 0.715152, 0.072175)),
        ToonHas(material, TOON_FEATURE_ALPHA_TEST));

    float ao = 1.0;
    if (ToonHas(material, TOON_FEATURE_OCCLUSION))
    {
        float occlusion = clamp(
            (toonOcclusionMap.SampleBias(toonOcclusionMapSampler, input.fragTexCoord, MATERIAL_MIP_BIAS).g - material.occlusion.x) / max(material.occlusion.y - material.occlusion.x, 1e-4),
            0.0, 1.0);
        ao = 1.0 + material.occlusion.z * (occlusion - 1.0);
    }

    float cel = ToonCelShade(material, NdotL, input.fragTexCoord);
    // Before the rim light, which is the sun's and goes with it.
    float sun = ToonApplyWorldLight(light, material, faceMask, input.fragWorldPosition, normalize(input.fragSmoothNormal), toCamera);

    // The face's shadow from its signed distance map: the light's angle about the face's up, against
    // the map (mirrored for light from the other side) read on the second UV set.
    if (ToonHas(material, TOON_FEATURE_FACE_SDF))
    {
        float3 forward = ToonFaceForward(material);
        float3 up = ToonFaceUp(material);
        // The face's side in this right-handed world (the export mirrored Unity's X).
        float3 side = cross(up, forward);
        float2 lightOnFace = float2(dot(side, L), dot(forward, L));
        lightOnFace = dot(lightOnFace, lightOnFace) > 1e-8 ? normalize(lightOnFace) : float2(0.0, 1.0);
        float2 uv = input.fragTexCoord1;
        float midPoint = material.faceSdf.x;
        if (lightOnFace.x < 0.0)
        {
            uv.x = midPoint - (uv.x - midPoint);
        }
        float distanceField = toonFaceShadowGradientMap.SampleBias(toonFaceShadowGradientMapSampler, uv, MATERIAL_MIP_BIAS).g + material.faceSdf.z;
        float threshold = 1.0 - (lightOnFace.y * 0.5 + 0.5);
        float softness = material.faceSdf.w;
        float faceLit = smoothstep(threshold - softness, threshold + softness, distanceField);
        float mask = faceMask * toonFaceShadowGradientMask.SampleBias(toonFaceShadowGradientMaskSampler, input.fragTexCoord1, MATERIAL_MIP_BIAS).g * material.faceSdf.y;
        float faceShadow = lerp(1.0, faceLit, mask);
        cel = lerp(cel * faceShadow, faceShadow, faceMask * material.faceSdf2.x);
    }

    // The sun's shadow, where the shader reads its own character shadow map.
    if (ToonHas(material, TOON_FEATURE_SELF_SHADOW) && light.castsShadow)
    {
        float shadow = EvaluateDirectionalShadow(input.fragWorldPosition, normalize(input.fragGeometryNormal) * faceSign);
        float facing = smoothstep(0.0, 1.0, clamp((clamp(NdotL, 0.0, 1.0) - 0.1) * 10.0, 0.0, 1.0));
        float strength = material.shadowHsv.w * lerp(material.skinShadowTint.a, material.faceShadowTint.a, faceMask);
        cel *= 1.0 + strength * (shadow * facing - 1.0);
    }

    float3 rimColor = lerp(float3(1.0), albedo, material.depthTexRim.x) * material.rimLight.rgb * (material.rimLight.a / 6.0) * min(light.color, float3(2.0));
    float rimMask = 0.0;
    float depthShadow = 1.0;
    if (ToonHas(material, TOON_FEATURE_DEPTH_TEX_RIM_SHADOW))
    {
        // The rim light and contact shadow from the characters' depth: a neighbour toward the light
        // much farther than this pixel lights its rim, one much nearer shades it. The reach is the
        // shader's: a fraction of the screen's height over the view depth, narrower on the face.
        float selfDepth = -mul(ubo.view, float4(input.fragWorldPosition, 1.0)).z;
        float2 size = float2(TextureSize(toonLinearDepth, 0));
        float reach = 0.0054 * abs(ubo.proj[1][1]) / max(selfDepth, 1e-3) * material.depthTexRim.y * (1.0 - 0.33334 * faceMask) * size.y * 0.707;
        // Fades out toward the bottom of the screen.
        float fade = smoothstep(0.0, 1.0, clamp((1.0 - input.fragCoord.y / size.y) * 20.0, 0.0, 1.0));
        float2 rimOffset = ScreenDirection(input.fragWorldPosition, L) * reach * material.depthTexRim.z;
        rimOffset.y *= fade;
        float3 shadowDirection = lerp(L, normalize(input.fragSmoothNormal), material.depthTexShadow.z);
        float2 shadowOffset = ScreenDirection(input.fragWorldPosition, shadowDirection) * reach * material.depthTexShadow.x;
        shadowOffset.y *= fade;
        float extend = reach / 0.707 * material.depthTexRim.z * material.depthTexRim.w;

        float threshold = selfDepth + material.depthTexRim2.z + clamp(0.05 + material.depthTexRim2.x, 0.0, 1.0) +
                          material.depthTexShadowTintFace.w * faceMask;
        float rimScale = 10.0 / material.depthTexRim2.y;
        float2 pixel = input.fragCoord.xy;
        rimMask = clamp((ToonDepthAt(pixel + rimOffset) - threshold) * rimScale, 0.0, 1.0);
        // Three more taps around the first close the dotted gaps a single tap leaves on thin edges.
        rimMask = min(rimMask, clamp((ToonDepthAt(pixel + rimOffset + float2(-extend, 0.0)) - threshold) * rimScale, 0.0, 1.0));
        rimMask = min(rimMask, clamp((ToonDepthAt(pixel + rimOffset + float2(extend, 0.0)) - threshold) * rimScale, 0.0, 1.0));
        rimMask = min(rimMask, clamp((ToonDepthAt(pixel + rimOffset + float2(0.0, extend)) - threshold) * rimScale, 0.0, 1.0));

        float occluder = ToonDepthAt(pixel + shadowOffset);
        depthShadow = lerp(1.0, clamp((occluder - (selfDepth - (0.03 - 0.02 * faceMask))) * 50.0, 0.0, 1.0), material.depthTexShadow.y);
        cel *= depthShadow;
    }
    else
    {
        // The classic rim: grazing views, toward the light.
        float rim = (1.0 - NdotV) * (clamp(NdotL, 0.0, 1.0) * 0.25 + 0.75) * (NdotL > 0.0 ? 1.0 : 0.0);
        rimMask = smoothstep(0.68, 0.72, rim);
    }

    float lit = clamp(cel * ao * sun, 0.0, 1.0);
    float3 color = lerp(ToonShadowColor(material, albedo, faceMask, skinMask), albedo, lit) * light.color;
    if (ToonHas(material, TOON_FEATURE_DEPTH_TEX_RIM_SHADOW))
    {
        float3 shadowTint = lerp(
            material.depthTexShadowTint.rgb * material.depthTexShadow.w,
            material.depthTexShadowTintFace.rgb * material.depthTexShadowTint.a,
            faceMask);
        color *= lerp(shadowTint, float3(1.0), depthShadow);
    }
    color += rimColor * rimMask * sun;
    // The character's own rim (its render controller's), over every surface.
    color += pow(1.0 - NdotV, max(material.headPosition.w, 1.0)) * material.characterRim.rgb;

    color = ApplyAerialPerspective(color * light.luminance, input.fragWorldPosition) * ubo.exposure.x * drawData.exposureScale;
    output.outColor = float4(color, ToonHas(material, TOON_FEATURE_TRANSPARENT) ? alpha : 1.0);
    output.outVelocity = ToonVelocity(input, output.outColor.a);
    return output;
}
