#version 450
#extension GL_GOOGLE_include_directive : require

layout(constant_id = 0) const bool kAlphaMask = false;

#include "scene_common.glsl"

layout(push_constant) uniform DrawConstants
{
    mat4 model;
    vec4 baseColorFactor;
    vec3 emissiveFactor;
    float alphaCutoff;
    vec4 surfaceFactors;
    vec4 nodeGraphFactors;
}
drawData;

layout(set = 1, binding = 0) uniform sampler2D baseColorTexture;
layout(set = 1, binding = 1) uniform sampler2D normalTexture;
layout(set = 1, binding = 2) uniform sampler2D metallicTexture;
layout(set = 1, binding = 3) uniform sampler2D roughnessTexture;
layout(set = 1, binding = 4) uniform sampler2D occlusionTexture;
layout(set = 1, binding = 5) uniform sampler2D emissiveTexture;
layout(set = 1, binding = 6) uniform sampler2D secondaryBaseColorTexture;
layout(set = 1, binding = 7) uniform sampler2D secondaryNormalTexture;
layout(set = 1, binding = 8) uniform sampler2D secondaryMetallicTexture;
layout(set = 1, binding = 9) uniform sampler2D secondaryRoughnessTexture;
layout(set = 1, binding = 10) uniform sampler2D secondaryOcclusionTexture;
layout(set = 1, binding = 11) uniform sampler2D secondaryEmissiveTexture;
layout(set = 1, binding = 12) uniform sampler2D blendMaskTexture;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 fragWorldNormal;
layout(location = 3) in vec4 fragWorldTangent;
layout(location = 4) in vec3 fragWorldPosition;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

// ---------------------------------------------------------------------------
// PBR microfacet BRDF helpers
// ---------------------------------------------------------------------------

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdH = max(dot(N, H), 0.0);
    float NdH2 = NdH * NdH;
    float denom = NdH2 * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 0.0001);
}

float GeometrySchlickGGX(float NdV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdV / max(NdV * (1.0 - k) + k, 0.0001);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdV = max(dot(N, V), 0.0);
    float NdL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdV, roughness) * GeometrySchlickGGX(NdL, roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ---------------------------------------------------------------------------
// Cook-Torrance BRDF contribution for one light sample
// Returns outgoing radiance.
// ---------------------------------------------------------------------------
vec3 EvaluateBRDF(
    vec3 N, vec3 V, vec3 L,
    vec3 albedo, float metallic, float roughness,
    vec3 radiance)
{
    float NdL = max(dot(N, L), 0.0);
    if (NdL <= 0.0)
        return vec3(0.0);

    vec3 H = normalize(V + L);
    float NdV = max(dot(N, V), 0.0);
    float HdV = max(dot(H, V), 0.0);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = FresnelSchlick(HdV, F0);
    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);

    vec3 specular = (D * G * F) / max(4.0 * NdV * NdL, 0.0001);

    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diffuse = kD * albedo / PI;

    return (diffuse + specular) * radiance * NdL;
}

// ---------------------------------------------------------------------------
// Distance attenuation: inverse square, windowed to zero at the light's range
// ---------------------------------------------------------------------------
// The windowing half on its own: 1 near the light, falling smoothly to 0 at the range.
float RangeWindow(float distance, float range)
{
    float ratio = distance / max(range, 0.001);
    float ratio4 = ratio * ratio * ratio * ratio;
    float num = clamp(1.0 - ratio4, 0.0, 1.0);
    return num * num;
}

// Physical 1 / d^2. The floor stops the singularity at the light's position: 1 cm, far below any
// distance a lit surface sits at. The "+ 1" some engines put in the denominator instead halves the
// light at one metre and cuts it to a tenth at thirty centimetres, which does not hold up once
// intensities are in lumens.
const float kMinLightDistanceSquared = 1e-4;

float SmoothDistanceAttenuation(float distance, float range)
{
    return RangeWindow(distance, range) / max(distance * distance, kMinLightDistanceSquared);
}

// ---------------------------------------------------------------------------
// Rectangular area light
// ---------------------------------------------------------------------------

// The cosine-weighted solid angle of a polygon, the integral of dot(N, w) over the directions w
// it covers, in Lambert's closed form: one term per edge. Corners must wind counterclockwise
// about the light's emitting normal. Polygons crossing the receiver's horizon are not clipped,
// which overestimates slightly there; the clamp only keeps the result from going negative.
float RectangleFormFactor(vec3 worldPos, vec3 N, vec3 corners[4])
{
    float sum = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        vec3 a = normalize(corners[i] - worldPos);
        vec3 b = normalize(corners[(i + 1) % 4] - worldPos);
        vec3 edgeNormal = cross(b, a);
        float sinAngle = length(edgeNormal);
        if (sinAngle > 1e-7)
        {
            // atan rather than acos(dot): for a small or distant light the edge subtends a tiny
            // angle, where acos near 1 loses most of its precision in fp32.
            sum += atan(sinAngle, dot(a, b)) * dot(edgeNormal / sinAngle, N);
        }
    }
    return max(0.5 * sum, 0.0);
}

// A one-sided Lambertian rectangle. Diffuse uses the exact irradiance from RectangleFormFactor.
// Specular uses a representative point, the point on the rectangle closest to the reflection
// ray, renormalized for the angle the light subtends so the enlarged highlight does not add
// energy. As the rectangle shrinks this converges to a point light emitting
// flux / pi along its normal with a cosine falloff.
vec3 EvaluateAreaLight(
    SceneLightData light,
    vec3 worldPos,
    vec3 N, vec3 V,
    vec3 albedo, float metallic, float roughness)
{
    vec3 center = light.positionAndRange.xyz;
    vec3 lightNormal = normalize(light.directionAndType.xyz);
    vec3 rightAxis = normalize(light.areaRightAxis.xyz);
    vec3 upAxis = cross(lightNormal, rightAxis);
    vec2 halfSize = 0.5 * max(light.spotAndArea.zw, vec2(0.001));

    // One sided: the back of the rectangle emits nothing.
    if (dot(worldPos - center, lightNormal) <= 0.0)
        return vec3(0.0);

    vec3 corners[4];
    corners[0] = center - rightAxis * halfSize.x - upAxis * halfSize.y;
    corners[1] = center + rightAxis * halfSize.x - upAxis * halfSize.y;
    corners[2] = center + rightAxis * halfSize.x + upAxis * halfSize.y;
    corners[3] = center - rightAxis * halfSize.x + upAxis * halfSize.y;

    float formFactor = RectangleFormFactor(worldPos, N, corners);
    if (formFactor <= 0.0)
        return vec3(0.0);

    // Lumens to luminance for a Lambertian emitter: flux / (pi * area).
    float area = 4.0 * halfSize.x * halfSize.y;
    vec3 luminance = light.colorAndIntensity.rgb * (light.colorAndIntensity.w / (PI * area));
    float window = RangeWindow(distance(worldPos, center), light.positionAndRange.w);
    vec3 irradiance = luminance * formFactor * window;

    // Representative point: where the reflection ray meets the light's plane, clamped into the
    // rectangle. A ray that never reaches the plane falls back to the receiver's projection onto it.
    vec3 R = reflect(-V, N);
    float rayDotNormal = dot(R, lightNormal);
    vec3 planePoint = rayDotNormal < -1e-4
                          ? worldPos + R * (dot(center - worldPos, lightNormal) / rayDotNormal)
                          : worldPos - lightNormal * dot(worldPos - center, lightNormal);
    vec3 local = planePoint - center;
    vec3 closest = center +
                   rightAxis * clamp(dot(local, rightAxis), -halfSize.x, halfSize.x) +
                   upAxis * clamp(dot(local, upAxis), -halfSize.y, halfSize.y);

    vec3 toLight = closest - worldPos;
    float lightDistance = max(length(toLight), 0.0001);
    vec3 L = toLight / lightDistance;

    vec3 H = normalize(V + L);
    float NdV = max(dot(N, V), 0.0);
    float NdL = max(dot(N, L), 0.0);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

    // Moving L to the representative point already spreads the highlight over the light's shape,
    // so the lobe itself is left alone and only renormalized: (alpha / widened)^2, with alpha
    // widened by the half angle of a disc with the rectangle's area. Without it the highlight's
    // peak would stay a point light's while its footprint grew, adding energy.
    float alpha = roughness * roughness;
    float equivalentRadius = sqrt(area / PI);
    float widenedAlpha = clamp(alpha + equivalentRadius / (2.0 * lightDistance), alpha, 1.0);
    float energyNormalization = (alpha * alpha) / (widenedAlpha * widenedAlpha);

    float D = DistributionGGX(N, H, roughness) * energyNormalization;
    float G = GeometrySmith(N, V, L, roughness);
    vec3 specular = NdL > 0.0 ? (D * G * F) / max(4.0 * NdV * NdL, 0.0001) : vec3(0.0);

    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diffuse = kD * albedo / PI;

    return (diffuse + specular) * irradiance;
}

// ---------------------------------------------------------------------------
// Uniform ambient
// ---------------------------------------------------------------------------

// Karis' analytic fit of the split-sum environment BRDF ("Physically Based Shading on Mobile",
// 2014). Returns the scale and bias that turn F0 into the directional albedo of the GGX lobe:
// specular albedo = F0 * x + y.
vec2 EnvironmentBrdfApprox(float roughness, float NdV)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// Outgoing radiance from an environment of the same luminance in every direction. For that
// environment the split sum is exact apart from the BRDF fit: the prefiltered radiance is the
// luminance itself whatever the roughness. Metals get only the specular lobe, tinted by their F0,
// and the diffuse lobe keeps whatever energy the specular one did not reflect.
vec3 EvaluateUniformAmbient(
    vec3 N, vec3 V,
    vec3 albedo, float metallic, float roughness,
    vec3 luminance)
{
    float NdV = max(dot(N, V), 0.0);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec2 environmentBrdf = EnvironmentBrdfApprox(roughness, NdV);
    vec3 specularAlbedo = F0 * environmentBrdf.x + environmentBrdf.y;
    vec3 diffuseAlbedo = albedo * (1.0 - metallic) * (vec3(1.0) - specularAlbedo);
    return (diffuseAlbedo + specularAlbedo) * luminance;
}

// ---------------------------------------------------------------------------
// Per-light contribution
// ---------------------------------------------------------------------------
vec3 EvaluateSceneLight(
    SceneLightData light,
    vec3 worldPos,
    vec3 N, vec3 V,
    vec3 albedo, float metallic, float roughness)
{
    int lightType = int(light.directionAndType.w);

    if (lightType == LIGHT_AMBIENT)
        return vec3(0.0); // handled as ambient term below

    vec3 L;
    vec3 radiance;

    if (lightType == LIGHT_DIRECTIONAL)
    {
        // Direction stored is the world direction the light travels; negate for L.
        L = normalize(-light.directionAndType.xyz);
        // Intensity is in lux (irradiance on a surface).
        radiance = light.colorAndIntensity.rgb * light.colorAndIntensity.w;
    }
    else if (lightType == LIGHT_POINT)
    {
        vec3 toLight = light.positionAndRange.xyz - worldPos;
        float dist = length(toLight);
        L = toLight / max(dist, 0.0001);
        float att = SmoothDistanceAttenuation(dist, light.positionAndRange.w);
        // Lumens → lux at surface: I / (4π), then attenuate.
        radiance = light.colorAndIntensity.rgb * (light.colorAndIntensity.w / (4.0 * PI)) * att;
    }
    else if (lightType == LIGHT_SPOT)
    {
        vec3 toLight = light.positionAndRange.xyz - worldPos;
        float dist = length(toLight);
        L = toLight / max(dist, 0.0001);

        float att = SmoothDistanceAttenuation(dist, light.positionAndRange.w);

        vec3 spotDir = normalize(light.directionAndType.xyz);
        float cosAngle = dot(-L, spotDir);
        float innerCos = light.spotAndArea.x;
        float outerCos = light.spotAndArea.y;
        float spotAtt = clamp((cosAngle - outerCos) / max(innerCos - outerCos, 0.0001), 0.0, 1.0);
        spotAtt *= spotAtt;

        // Lumens to candela over the solid angle of the outer cone. Light keeps reaching out to the
        // outer angle, so dividing by the inner cone alone would emit more flux than authored, and
        // more the softer the edge.
        float coneOmega = max(2.0 * PI * (1.0 - outerCos), 0.0001);
        radiance = light.colorAndIntensity.rgb * (light.colorAndIntensity.w / coneOmega) * att * spotAtt;
    }
    else if (lightType == LIGHT_AREA)
    {
        // Integrates over the rectangle itself, so it does not go through EvaluateBRDF.
        return EvaluateAreaLight(light, worldPos, N, V, albedo, metallic, roughness);
    }
    else
    {
        return vec3(0.0);
    }

    return EvaluateBRDF(N, V, L, albedo, metallic, roughness, radiance);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
void main()
{
    // ---- Blend mask & blend weight ----------------------------------------
    float blendMask = texture(blendMaskTexture, fragTexCoord).r;
    float blendWeight = clamp(
        mix(0.0, drawData.nodeGraphFactors.y, clamp(drawData.nodeGraphFactors.x, 0.0, 1.0)) * blendMask,
        0.0, 1.0);

    // ---- Albedo -----------------------------------------------------------
    vec4 primaryBaseColor = texture(baseColorTexture, fragTexCoord);
    vec4 secondaryBaseColor = texture(secondaryBaseColorTexture, fragTexCoord);
    vec4 sampledBaseColor = mix(primaryBaseColor, secondaryBaseColor, blendWeight);
    vec4 albedo = sampledBaseColor * vec4(fragColor, 1.0) * drawData.baseColorFactor;

    if (kAlphaMask && albedo.a < drawData.alphaCutoff)
        discard;

    // ---- Normal -----------------------------------------------------------
    // A back face is only rasterized by a double-sided pipeline, and it is seen from the side the
    // vertex normal points away from. Mirroring the whole tangent frame, bitangent included, shades
    // it as the front face would be from the other side, with the normal map's perturbation
    // mirrored along with it. Single-sided pipelines cull back faces, so this is a no-op for them.
    float faceSign = gl_FrontFacing ? 1.0 : -1.0;
    vec3 geoNormal = normalize(fragWorldNormal) * faceSign;
    vec3 faceTangent = fragWorldTangent.xyz * faceSign;
    vec3 tangent = normalize(faceTangent - geoNormal * dot(geoNormal, faceTangent));
    // cross(-N, -T) == cross(N, T), so the flip has to be applied to the bitangent explicitly.
    vec3 bitangent = normalize(cross(geoNormal, tangent) * fragWorldTangent.w) * faceSign;
    mat3 TBN = mat3(tangent, bitangent, geoNormal);

    vec3 nrmPrimary = texture(normalTexture, fragTexCoord).xyz * 2.0 - 1.0;
    vec3 nrmSecondary = texture(secondaryNormalTexture, fragTexCoord).xyz * 2.0 - 1.0;
    vec3 nrmSample = normalize(mix(nrmPrimary, nrmSecondary, blendWeight));
    nrmSample.xy *= drawData.surfaceFactors.z; // normal scale
    vec3 N = normalize(TBN * nrmSample);

    // ---- PBR factors ------------------------------------------------------
    float metallicSample = mix(
        texture(metallicTexture, fragTexCoord).b,
        texture(secondaryMetallicTexture, fragTexCoord).b,
        blendWeight);
    float roughnessSample = mix(
        texture(roughnessTexture, fragTexCoord).g,
        texture(secondaryRoughnessTexture, fragTexCoord).g,
        blendWeight);
    float aoSample = mix(
        texture(occlusionTexture, fragTexCoord).r,
        texture(secondaryOcclusionTexture, fragTexCoord).r,
        blendWeight);
    vec3 emissiveSample = mix(
        texture(emissiveTexture, fragTexCoord).rgb,
        texture(secondaryEmissiveTexture, fragTexCoord).rgb,
        blendWeight);

    float metallic = clamp(drawData.surfaceFactors.x * metallicSample, 0.0, 1.0);
    float roughness = clamp(drawData.surfaceFactors.y * roughnessSample, 0.04, 1.0);
    float ao = mix(1.0, aoSample, clamp(drawData.surfaceFactors.w, 0.0, 1.0));

    vec3 V = normalize(ubo.cameraWorldPosition.xyz - fragWorldPosition);

    // ---- Ambient ----------------------------------------------------------
    // The ambient light is a uniform environment of this luminance. The CPU has already summed the
    // scene's Ambient lights into it, or put the fallback there when there are none.
    vec3 ambient = EvaluateUniformAmbient(N, V, albedo.rgb, metallic, roughness, ubo.ambientLuminance.rgb) * ao;

    // ---- Direct lighting --------------------------------------------------
    uint lightCount = ubo.sceneLightCount.x;
    vec3 directAccum = vec3(0.0);
    for (uint i = 0u; i < lightCount; ++i)
    {
        directAccum += EvaluateSceneLight(
            ubo.lights[i],
            fragWorldPosition,
            N, V,
            albedo.rgb, metallic, roughness);
    }

    // ---- Combine ----------------------------------------------------------
    vec3 emissive = emissiveSample * drawData.emissiveFactor;
    vec3 color = ambient + directAccum + emissive;

    // Tone mapping happens in the tonemap pass, which is the only consumer of this target. This
    // shader writes linear radiance.
    outColor = vec4(color, albedo.a);
}
