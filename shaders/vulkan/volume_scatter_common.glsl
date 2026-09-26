// KHR_materials_volume_scatter as the Khronos glTF Sample Viewer renders it: a screen-space Burley
// diffusion (Blender's implementation of Christensen and Burley 2015) of the light the scatter
// pre-pass (VulkanScatterPass) gathered into the surface. triangle.frag reads the pre-pass through
// these rules.
//
// Compiled twice, like transmission_common.glsl: by glslc from triangle.frag and by
// tests/volume_scatter_tests.cpp inside a namespace with `using namespace glm`, so it keeps to the
// subset both accept (f-suffixed literals, no out parameters).
#ifndef VOLUME_SCATTER_COMMON_GLSL
#define VOLUME_SCATTER_COMMON_GLSL

// The viewer's disk: this many samples (gltfMaterial.scatterSampleCount).
const int kScatterSampleCount = 55;
const float kScatterPi = 3.14159265358979f;
// The share of the Burley profile's energy inside the radius the samples reach (burleySample).
const float kBurleyCdfMaximum = 0.9963790093708328f;

// The single-scatter albedo for a multi-scatter one, as the extension defines it (Kulla and Conty 2017):
// the share of the medium's extinction that scatters rather than absorbs.
vec3 MultiToSingleScatter(vec3 multiscatter)
{
    vec3 s = 4.09712f + 4.20863f * multiscatter -
             sqrt(9.59217f + 41.6808f * multiscatter + 17.7126f * multiscatter * multiscatter);
    return 1.0f - s * s;
}

// Burley's shape parameter d for a scatter radius, at a white albedo (burley_setup).
vec3 BurleyShape(vec3 radius)
{
    // 1.9 - albedo + 3.5 (albedo - 0.8)^2 at albedo 1.
    return radius * (0.25f / kScatterPi) / 1.04f;
}

// The diffusion profile at distance r, up to a constant factor (burley_eval): the gather normalizes
// by the summed weights.
vec3 BurleyProfile(vec3 d, float r)
{
    vec3 exp3 = exp(-r / (3.0f * d));
    vec3 exp1 = exp3 * exp3 * exp3;
    return (exp1 + exp3) / (4.0f * d);
}

// The i-th of the viewer's precomputed samples (computeScatterSamples) for a unit scatter radius:
// x the angle (golden angle steps), y the radius (the profile's inverse CDF at (i + 0.5) / count, by
// Newton's method), z one over the profile's pdf there.
vec3 BurleyScatterSample(int index)
{
    float d = BurleyShape(vec3(1.0f)).x;
    float theta = kScatterPi * (3.0f - sqrt(5.0f)) * float(index) + kScatterPi;
    float x = (0.5f + float(index)) / float(kScatterSampleCount) * kBurleyCdfMaximum;
    float r = x <= 0.9f ? exp(x * x * 2.4f) - 1.0f : 15.0f;
    for (int iteration = 0; iteration < 10; ++iteration)
    {
        float exp3 = exp(-r / 3.0f);
        float exp1 = exp3 * exp3 * exp3;
        float f = 1.0f - 0.25f * exp1 - 0.75f * exp3 - x;
        float slope = 0.25f * exp1 + 0.25f * exp3;
        if (abs(f) < 1e-6f || slope == 0.0f)
        {
            break;
        }
        r = max(r - f / slope, 0.0f);
    }
    r *= d;
    float exp3 = exp(-r / (3.0f * d));
    float pdf = (exp3 * exp3 * exp3 + exp3) / (8.0f * kScatterPi * d) / kBurleyCdfMaximum;
    return vec3(theta, r, 1.0f / pdf);
}

// The smallest sample radius, the first (u_MinRadius): the viewer keeps every channel's scatter radius
// at least this share of the largest, since the samples cannot resolve less.
float BurleyMinimumRadius()
{
    return max(BurleyScatterSample(0).y, 0.00001f);
}

#endif
