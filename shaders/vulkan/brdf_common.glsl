// BRDF terms shared by pbr_common.glsl, environment_brdf.cpp's reference quadrature in the tests and
// tests/brdf_tests.cpp, which compiles this file as C++. Written, like ssr_common.glsl, in the subset
// GLSL and C++/GLM both accept: every float literal carries the f suffix, no out parameters, const
// only for literal initializers.

#ifndef BRDF_COMMON_GLSL
#define BRDF_COMMON_GLSL

const float kBrdfPi = 3.14159265f;

// The height-correlated Smith visibility, G / (4 N.L N.V) (Heitz 2014), for alpha = roughness^2.
// The DFG table integrates the same term, so the energy compensation matches the lobe drawn.
float VisibilitySmithGgxCorrelated(float NdV, float NdL, float alpha)
{
    float a2 = alpha * alpha;
    float ggxV = NdL * sqrt(NdV * NdV * (1.0f - a2) + a2);
    float ggxL = NdV * sqrt(NdL * NdL * (1.0f - a2) + a2);
    return 0.5f / max(ggxV + ggxL, 1e-7f);
}

// Burley's diffuse (Disney 2012) in Frostbite's energy-normalised form (Lagarde and de Rousiers
// 2014), without the albedo: multiply by albedo. Rough surfaces brighten toward grazing angles
// (retroreflection), smooth ones darken there; the energy factor keeps the peak from exceeding
// Lambert's.
float BurleyDiffuse(float NdV, float NdL, float LdH, float roughness)
{
    float energyBias = 0.5f * roughness;
    float energyFactor = mix(1.0f, 1.0f / 1.51f, roughness);
    float f90 = energyBias + 2.0f * LdH * LdH * roughness;
    float lightScatter = 1.0f + (f90 - 1.0f) * pow(1.0f - NdL, 5.0f);
    float viewScatter = 1.0f + (f90 - 1.0f) * pow(1.0f - NdV, 5.0f);
    return lightScatter * viewScatter * energyFactor / kBrdfPi;
}

// How much of the environment a reflection vector still sees above the geometric surface: a
// normal map can tilt R below it, where the surface itself blocks the environment. Squared fade,
// as Jimenez et al. 2016 and Frostbite have it.
float HorizonSpecularOcclusion(vec3 R, vec3 geometricNormal)
{
    float horizon = clamp(1.0f + dot(R, geometricNormal), 0.0f, 1.0f);
    return horizon * horizon;
}

// The direction to the point of a disk light (the sun) closest to the reflection vector R: R itself
// inside the disk, the disk's edge toward R outside (Filament). toCentre is the unit direction to
// the disk's centre, cosRadius and sinRadius its angular radius.
vec3 DiskLightSpecularDirection(vec3 toCentre, vec3 R, float cosRadius, float sinRadius)
{
    float DdotR = dot(toCentre, R);
    if (DdotR >= cosRadius)
    {
        return R;
    }
    vec3 S = R - DdotR * toCentre;
    float lengthS = length(S);
    if (lengthS < 1e-6f)
    {
        return toCentre;
    }
    return normalize(cosRadius * toCentre + (sinRadius / lengthS) * S);
}

// The unnormalised vector to the point of a sphere light closest to the reflection ray (Karis
// 2013): toLight is the vector from the surface to the sphere's centre. Its length is the distance
// to that point.
vec3 SphereLightSpecularVector(vec3 toLight, vec3 R, float radius)
{
    vec3 centreToRay = dot(toLight, R) * R - toLight;
    float lengthToRay = length(centreToRay);
    return toLight + centreToRay * clamp(radius / max(lengthToRay, 1e-6f), 0.0f, 1.0f);
}

// What a GGX lobe of this alpha is scaled by when its light is widened by a source subtending
// halfAngle (a tangent or sine): the lobe's peak falls as it spreads over the source,
// (alpha / alpha')^2 with alpha' = saturate(alpha + halfAngle / 2).
float SourceSizeNormalization(float alpha, float halfAngle)
{
    float widened = clamp(alpha + 0.5f * halfAngle, alpha, 1.0f);
    return (alpha * alpha) / max(widened * widened, 1e-8f);
}

#endif
