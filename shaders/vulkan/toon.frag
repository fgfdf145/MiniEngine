#version 450
#extension GL_GOOGLE_include_directive : require

// The toon pass (toon_pass.h): an anime character's cel shading as AnimateApp's "Anime Forward"
// pass computes it, or under kOutline its outline's colour ("Anime Outline"). See
// docs/design/2026-10-07-yuki-toon-shading-design.md for the shader this follows.
//
// The shading is in the shader's own units, where the main light is about 1 (Unity's directional
// light at intensity 1, its colour ToonLight.color) and the result is the albedo where lit; the
// light's luminance then takes it into the scene's radiance, the pre-exposure into the HDR target.
layout(constant_id = 0) const bool kOutline = false;

#include "scene_common.glsl"
#include "atmosphere_sampling.glsl"
#include "pbr_common.glsl"
#include "toon_common.glsl"

// The toon prepass's targets (VulkanToonPass's set 3): the characters' linear view depth and the eye
// mask, fetched by pixel.
layout(set = 3, binding = 0) uniform sampler2D toonLinearDepth;
layout(set = 3, binding = 1) uniform sampler2D toonMask;

layout(location = 0) in vec3 fragWorldPosition;
layout(location = 1) in vec3 fragWorldNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in vec2 fragTexCoord1;
layout(location = 4) in vec3 fragSmoothNormal;
layout(location = 5) in vec3 fragGeometryNormal;
layout(location = 6) flat in uint fragToonIndex;
layout(location = 7) in vec4 fragCurrClip;
layout(location = 8) in vec4 fragPrevClip;

layout(location = 0) out vec4 outColor;
// GBufferVelocity, as gbuffer.frag writes it: rg the motion in UV units (current - previous); b the
// coat normal's x, none here. Blended with the colour's alpha on a transparent surface.
layout(location = 1) out vec4 outVelocity;

vec4 ToonVelocity(float alpha)
{
    vec2 current = fragCurrClip.xy / fragCurrClip.w;
    vec2 previous = fragPrevClip.xy / fragPrevClip.w;
    return vec4((current - previous) * 0.5, 0.0, alpha);
}

// The main light as the shader sees it: where it comes from, its colour normalised to unit luminance
// (a white sun is 1, as Unity's directional light at intensity 1), and the luminance the toon
// colours are scaled by into the scene's radiance: what a white diffuse surface facing it would send
// (lux / pi). The shadow casting directional light, else the first, else the ambient light from the
// camera's side.
struct ToonLight
{
    vec3 direction;
    vec3 color;
    float luminance;
    bool castsShadow;
};

ToonLight ToonMainLight(vec3 toCamera)
{
    ToonLight light;
    int index = int(ubo.shadowParams.x);
    light.castsShadow = index >= 0;
    if (index < 0 && ubo.lightCounts.x > 0u)
    {
        index = 0;
    }
    vec3 radiance;
    if (index >= 0)
    {
        SceneLightData data = sceneLights.lights[index];
        light.direction = normalize(-data.directionAndType.xyz);
        radiance = data.colorAndIntensity.rgb * data.colorAndIntensity.w / PI;
    }
    else
    {
        light.direction = toCamera;
        radiance = ubo.ambientLuminance.rgb;
    }
    light.luminance = max(dot(radiance, vec3(0.2126, 0.7152, 0.0722)), 0.0);
    light.color = light.luminance > 0.0 ? radiance / light.luminance : vec3(0.0);
    return light;
}

float ToonDepthAt(vec2 pixel)
{
    ivec2 size = textureSize(toonLinearDepth, 0);
    return texelFetch(toonLinearDepth, clamp(ivec2(pixel), ivec2(0), size - 1), 0).r;
}

// Which way a world direction points on screen from this fragment, in pixels (unit length), however
// the projection is flipped; zero where it points at the camera.
vec2 ScreenDirection(vec3 worldPosition, vec3 direction)
{
    vec4 a = ubo.proj * ubo.view * vec4(worldPosition, 1.0);
    vec4 b = ubo.proj * ubo.view * vec4(worldPosition + direction * 0.01, 1.0);
    vec2 size = vec2(textureSize(toonLinearDepth, 0));
    vec2 delta = (b.xy / b.w - a.xy / a.w) * size;
    float length2 = dot(delta, delta);
    return length2 > 1e-12 ? delta * inversesqrt(length2) : vec2(0.0);
}

void main()
{
    ToonMaterial material = toonMaterials.materials[fragToonIndex];

    // The stencil the shader tests, from the eye mask: the front hair stays off the eyes, its redraw
    // keeps to them.
    float eyeMask = texelFetch(toonMask, ivec2(gl_FragCoord.xy), 0).r;
    if ((ToonHas(material, TOON_FEATURE_STENCIL_NOT_EQUAL) && eyeMask > 0.5) ||
        (ToonHas(material, TOON_FEATURE_STENCIL_EQUAL) && eyeMask < 0.5))
    {
        discard;
    }

    vec3 toCamera = normalize(ubo.cameraWorldPosition.xyz - fragWorldPosition);
    vec4 base = texture(toonBaseMap, fragTexCoord, MATERIAL_MIP_BIAS);
    float faceMask = ToonFaceMask(material, fragTexCoord);
    float skinMask = ToonSkinMask(material, fragTexCoord);
    ToonLight light = ToonMainLight(toCamera);

    if (kOutline)
    {
        // The hull's back faces carry the surface's normal as it is: no face flip, no face fix.
        vec3 N = normalize(fragGeometryNormal);
        if (ToonHas(material, TOON_FEATURE_ALPHA_TEST) && base.a * material.baseColor.a < material.mainLight.w)
        {
            discard;
        }
        vec3 albedo = ToonAlbedo(material, base.rgb, fragTexCoord, N, toCamera);
        float lit = clamp(ToonCelShade(material, dot(N, light.direction), fragTexCoord), 0.0, 1.0);
        vec3 color = mix(ToonShadowColor(material, albedo, faceMask, skinMask), albedo, lit) * light.color;
        color *= mix(material.outlineTint.rgb, material.outlineSkinOverride.rgb, material.outlineSkinOverride.a * skinMask);
        color = ApplyAerialPerspective(color * light.luminance, fragWorldPosition) * ubo.exposure.x * drawData.exposureScale;
        outColor = vec4(color, 1.0);
        outVelocity = ToonVelocity(1.0);
        return;
    }

    float faceSign = gl_FrontFacing ? 1.0 : -1.0;
    vec3 N = normalize(fragWorldNormal) * faceSign;
    vec3 L = light.direction;
    float NdotL = dot(N, L);
    float NdotV = clamp(dot(N, toCamera), 0.0, 1.0);

    vec3 albedo = ToonAlbedo(material, base.rgb, fragTexCoord, N, toCamera);
    vec3 additive = ToonAdditiveMatCap(material, albedo, fragTexCoord, N, toCamera);
    albedo += additive;
    float alpha = ToonAlpha(
        material, base.a * material.baseColor.a, fragTexCoord, toCamera, dot(additive, vec3(0.212673, 0.715152, 0.072175)),
        ToonHas(material, TOON_FEATURE_ALPHA_TEST));

    float ao = 1.0;
    if (ToonHas(material, TOON_FEATURE_OCCLUSION))
    {
        float occlusion = clamp(
            (texture(toonOcclusionMap, fragTexCoord, MATERIAL_MIP_BIAS).g - material.occlusion.x) / max(material.occlusion.y - material.occlusion.x, 1e-4),
            0.0, 1.0);
        ao = 1.0 + material.occlusion.z * (occlusion - 1.0);
    }

    float cel = ToonCelShade(material, NdotL, fragTexCoord);

    // The face's shadow from its signed distance map: the light's angle about the face's up, against
    // the map (mirrored for light from the other side) read on the second UV set.
    if (ToonHas(material, TOON_FEATURE_FACE_SDF))
    {
        vec3 forward = ToonFaceForward(material);
        vec3 up = ToonFaceUp(material);
        // The face's side in this right-handed world (the export mirrored Unity's X).
        vec3 side = cross(up, forward);
        vec2 lightOnFace = vec2(dot(side, L), dot(forward, L));
        lightOnFace = dot(lightOnFace, lightOnFace) > 1e-8 ? normalize(lightOnFace) : vec2(0.0, 1.0);
        vec2 uv = fragTexCoord1;
        float midPoint = material.faceSdf.x;
        if (lightOnFace.x < 0.0)
        {
            uv.x = midPoint - (uv.x - midPoint);
        }
        float distanceField = texture(toonFaceShadowGradientMap, uv, MATERIAL_MIP_BIAS).g + material.faceSdf.z;
        float threshold = 1.0 - (lightOnFace.y * 0.5 + 0.5);
        float softness = material.faceSdf.w;
        float faceLit = smoothstep(threshold - softness, threshold + softness, distanceField);
        float mask = faceMask * texture(toonFaceShadowGradientMask, fragTexCoord1, MATERIAL_MIP_BIAS).g * material.faceSdf.y;
        float faceShadow = mix(1.0, faceLit, mask);
        cel = mix(cel * faceShadow, faceShadow, faceMask * material.faceSdf2.x);
    }

    // The sun's shadow, where the shader reads its own character shadow map.
    if (ToonHas(material, TOON_FEATURE_SELF_SHADOW) && light.castsShadow)
    {
        float shadow = EvaluateDirectionalShadow(fragWorldPosition, normalize(fragGeometryNormal) * faceSign);
        float facing = smoothstep(0.0, 1.0, clamp((clamp(NdotL, 0.0, 1.0) - 0.1) * 10.0, 0.0, 1.0));
        float strength = material.shadowHsv.w * mix(material.skinShadowTint.a, material.faceShadowTint.a, faceMask);
        cel *= 1.0 + strength * (shadow * facing - 1.0);
    }

    vec3 rimColor = mix(vec3(1.0), albedo, material.depthTexRim.x) * material.rimLight.rgb * (material.rimLight.a / 6.0) * min(light.color, vec3(2.0));
    float rimMask = 0.0;
    float depthShadow = 1.0;
    if (ToonHas(material, TOON_FEATURE_DEPTH_TEX_RIM_SHADOW))
    {
        // The rim light and contact shadow from the characters' depth: a neighbour toward the light
        // much farther than this pixel lights its rim, one much nearer shades it. The reach is the
        // shader's: a fraction of the screen's height over the view depth, narrower on the face.
        float selfDepth = -(ubo.view * vec4(fragWorldPosition, 1.0)).z;
        vec2 size = vec2(textureSize(toonLinearDepth, 0));
        float reach = 0.0054 * abs(ubo.proj[1][1]) / max(selfDepth, 1e-3) * material.depthTexRim.y * (1.0 - 0.33334 * faceMask) * size.y * 0.707;
        // Fades out toward the bottom of the screen.
        float fade = smoothstep(0.0, 1.0, clamp((1.0 - gl_FragCoord.y / size.y) * 20.0, 0.0, 1.0));
        vec2 rimOffset = ScreenDirection(fragWorldPosition, L) * reach * material.depthTexRim.z;
        rimOffset.y *= fade;
        vec3 shadowDirection = mix(L, normalize(fragSmoothNormal), material.depthTexShadow.z);
        vec2 shadowOffset = ScreenDirection(fragWorldPosition, shadowDirection) * reach * material.depthTexShadow.x;
        shadowOffset.y *= fade;
        float extend = reach / 0.707 * material.depthTexRim.z * material.depthTexRim.w;

        float threshold = selfDepth + material.depthTexRim2.z + clamp(0.05 + material.depthTexRim2.x, 0.0, 1.0) +
                          material.depthTexShadowTintFace.w * faceMask;
        float rimScale = 10.0 / material.depthTexRim2.y;
        vec2 pixel = gl_FragCoord.xy;
        rimMask = clamp((ToonDepthAt(pixel + rimOffset) - threshold) * rimScale, 0.0, 1.0);
        // Three more taps around the first close the dotted gaps a single tap leaves on thin edges.
        rimMask = min(rimMask, clamp((ToonDepthAt(pixel + rimOffset + vec2(-extend, 0.0)) - threshold) * rimScale, 0.0, 1.0));
        rimMask = min(rimMask, clamp((ToonDepthAt(pixel + rimOffset + vec2(extend, 0.0)) - threshold) * rimScale, 0.0, 1.0));
        rimMask = min(rimMask, clamp((ToonDepthAt(pixel + rimOffset + vec2(0.0, extend)) - threshold) * rimScale, 0.0, 1.0));

        float occluder = ToonDepthAt(pixel + shadowOffset);
        depthShadow = mix(1.0, clamp((occluder - (selfDepth - (0.03 - 0.02 * faceMask))) * 50.0, 0.0, 1.0), material.depthTexShadow.y);
        cel *= depthShadow;
    }
    else
    {
        // The classic rim: grazing views, toward the light.
        float rim = (1.0 - NdotV) * (clamp(NdotL, 0.0, 1.0) * 0.25 + 0.75) * (NdotL > 0.0 ? 1.0 : 0.0);
        rimMask = smoothstep(0.68, 0.72, rim);
    }

    float lit = clamp(cel * ao, 0.0, 1.0);
    vec3 color = mix(ToonShadowColor(material, albedo, faceMask, skinMask), albedo, lit) * light.color;
    if (ToonHas(material, TOON_FEATURE_DEPTH_TEX_RIM_SHADOW))
    {
        vec3 shadowTint = mix(
            material.depthTexShadowTint.rgb * material.depthTexShadow.w,
            material.depthTexShadowTintFace.rgb * material.depthTexShadowTint.a,
            faceMask);
        color *= mix(shadowTint, vec3(1.0), depthShadow);
    }
    color += rimColor * rimMask;
    // The character's own rim (its render controller's), over every surface.
    color += pow(1.0 - NdotV, max(material.headPosition.w, 1.0)) * material.characterRim.rgb;

    color = ApplyAerialPerspective(color * light.luminance, fragWorldPosition) * ubo.exposure.x * drawData.exposureScale;
    outColor = vec4(color, ToonHas(material, TOON_FEATURE_TRANSPARENT) ? alpha : 1.0);
    outVelocity = ToonVelocity(outColor.a);
}
