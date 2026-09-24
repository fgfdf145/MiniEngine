// Screen-space reflection helpers shared by ssr_trace.comp, the lighting code in pbr_common.glsl
// and tests/ssr_tests.cpp, which compiles this file as C++. Written, like gt7_tonemap.glsl, in the
// subset GLSL and C++/GLM both accept: every float literal carries the f suffix, no out parameters,
// const only for literal initializers.

#ifndef SSR_COMMON_GLSL
#define SSR_COMMON_GLSL

// Where the trace starts handing a lobe over to the environment; SsrRoughnessFade reaches zero at
// the maximum roughness the settings give.
const float kSsrRoughnessFadeStart = 0.4f;
// The share of the screen at each edge over which a hit's confidence fades out.
const float kSsrEdgeFadeMargin = 0.1f;
const float kSsrPi = 3.14159265f;

// Lagarde and de Rousiers 2014, "Moving Frostbite to PBR": how much of the specular lobe the
// ambient occlusion also blocks. A narrow lobe seen head on escapes more than the cosine-weighted
// AO says; a wide one is occluded like the diffuse.
float SpecularOcclusion(float NdV, float ao, float roughness)
{
    return clamp(pow(NdV + ao, exp2(-16.0f * roughness - 1.0f)) - 1.0f + ao, 0.0f, 1.0f);
}

// 1 away from the screen's border, 0 at it: a hit near the edge is about to leave the screen, and
// cutting it off would draw a hard line across the reflection.
float SsrEdgeFade(vec2 uv)
{
    vec2 distanceToEdge = min(uv, vec2(1.0f) - uv);
    return smoothstep(0.0f, kSsrEdgeFadeMargin, min(distanceToEdge.x, distanceToEdge.y));
}

// 1 up to kSsrRoughnessFadeStart, 0 from maxRoughness on: rough lobes are what the prefiltered
// environment already integrates, and one ray a pixel is too noisy for them.
float SsrRoughnessFade(float roughness, float maxRoughness)
{
    if (maxRoughness <= kSsrRoughnessFadeStart)
    {
        return roughness < maxRoughness ? 1.0f : 0.0f;
    }
    return 1.0f - clamp((roughness - kSsrRoughnessFadeStart) / (maxRoughness - kSsrRoughnessFadeStart), 0.0f, 1.0f);
}

// A GGX half vector drawn from the distribution of normals visible from V (Heitz 2018, "Sampling
// the GGX Distribution of Visible Normals"), in world space around N. roughness is perceptual
// (alpha = roughness^2); u is uniform in [0, 1)^2. At roughness 0 it is N itself.
vec3 SampleGgxVisibleNormal(vec3 N, vec3 V, float roughness, vec2 u)
{
    vec3 up = abs(N.z) < 0.999f ? vec3(0.0f, 0.0f, 1.0f) : vec3(1.0f, 0.0f, 0.0f);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);
    float alpha = roughness * roughness;

    // The view in the tangent frame, kept above the surface.
    vec3 Ve = vec3(dot(V, T), dot(V, B), max(dot(V, N), 1e-4f));
    vec3 Vh = normalize(vec3(alpha * Ve.x, alpha * Ve.y, Ve.z));
    float lengthSquared = Vh.x * Vh.x + Vh.y * Vh.y;
    vec3 T1 = lengthSquared > 0.0f ? vec3(-Vh.y, Vh.x, 0.0f) / sqrt(lengthSquared) : vec3(1.0f, 0.0f, 0.0f);
    vec3 T2 = cross(Vh, T1);

    float r = sqrt(u.x);
    float phi = 2.0f * kSsrPi * u.y;
    float t1 = r * cos(phi);
    float s = 0.5f * (1.0f + Vh.z);
    float t2 = (1.0f - s) * sqrt(max(1.0f - t1 * t1, 0.0f)) + s * (r * sin(phi));
    vec3 Nh = t1 * T1 + t2 * T2 + sqrt(max(1.0f - t1 * t1 - t2 * t2, 0.0f)) * Vh;
    vec3 Ne = normalize(vec3(alpha * Nh.x, alpha * Nh.y, max(Nh.z, 0.0f)));
    return normalize(Ne.x * T + Ne.y * B + Ne.z * N);
}

#endif
