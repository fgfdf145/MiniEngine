// Linearly transformed cosines for polygonal lights (Heitz, Dupuy, Hill and Neubelt 2016), shared
// by pbr_common.slang and tests/ltc_tests.cpp, which compiles this file as C++. Written, like
// ssr_common.slang, in the subset Slang and C++/GLM both accept: every float literal carries the f
// suffix, no out parameters, static const only for literal initializers, matrices built with
// MatrixFromColumns / MatrixFromRows and multiplied with mul (engine/renderer/shader_cpp_compat.h
// gives C++ the same).

#ifndef LTC_COMMON_SLANG
#define LTC_COMMON_SLANG

#ifdef __SLANG__
#include "shader_helpers.slang"
#endif

// One edge's share of a polygon's vector form factor on the unit sphere: cross(v1, v2) times
// theta / (2 pi sin theta), with Hill and Heitz's rational fit of that factor (SIGGRAPH 2017
// course), which stays accurate where acos loses precision near 1. The 1 / (2 pi) is inside the
// fit: it tends to 1 / (2 pi), not 1, as theta goes to 0.
float3 LtcIntegrateEdge(float3 v1, float3 v2)
{
    float x = dot(v1, v2);
    float y = abs(x);
    float a = 0.8543985f + (0.4965155f + 0.0145206f * y) * y;
    float b = 3.4175940f + (4.1616724f + y) * y;
    float v = a / b;
    float thetaOverSinTheta = x > 0.0f ? v : 0.5f / sqrt(max(1.0f - x * x, 1e-7f)) - v;
    return cross(v1, v2) * thetaOverSinTheta;
}

// A convex polygon of up to five vertices: a quad clipped by a plane gains at most one.
struct LtcPolygon
{
    float3 v[5];
    int count;
};

// Clips a quad, in the cosine's space, to the upper hemisphere z >= 0 (Sutherland-Hodgman).
LtcPolygon LtcClipToHorizon(float3 q0, float3 q1, float3 q2, float3 q3)
{
    float3 q[4];
    q[0] = q0;
    q[1] = q1;
    q[2] = q2;
    q[3] = q3;
    LtcPolygon polygon;
    polygon.count = 0;
    for (int i = 0; i < 5; ++i)
    {
        polygon.v[i] = float3(0.0f);
    }
    for (int i = 0; i < 4; ++i)
    {
        float3 a = q[i];
        float3 b = q[(i + 1) % 4];
        if (a.z >= 0.0f)
        {
            polygon.v[polygon.count] = a;
            polygon.count += 1;
        }
        if ((a.z >= 0.0f) != (b.z >= 0.0f))
        {
            float t = a.z / (a.z - b.z);
            polygon.v[polygon.count] = a + t * (b - a);
            polygon.count += 1;
        }
    }
    return polygon;
}

// The integral of the clamped cosine distribution over a quad whose corners are given in the
// cosine's space (after the inverse LTC matrix): the fraction of a unit-albedo lobe the quad
// covers. The quad may wind either way; the lights are one-sided and their back is culled before.
float LtcIntegrateQuad(float3 q0, float3 q1, float3 q2, float3 q3)
{
    LtcPolygon polygon = LtcClipToHorizon(q0, q1, q2, q3);
    if (polygon.count < 3)
    {
        return 0.0f;
    }
    float sum = 0.0f;
    for (int i = 0; i < polygon.count; ++i)
    {
        float3 a = normalize(polygon.v[i]);
        float3 b = normalize(polygon.v[(i + 1) % polygon.count]);
        sum += LtcIntegrateEdge(a, b).z;
    }
    return abs(sum);
}

// The inverse LTC matrix from its table texel, columns (x, 0, y), (0, 1, 0) and (z, 0, w), in the
// frame (T1, T2, N) with T1 along V's projection onto the surface.
float3x3 LtcInverseMatrix(float4 texel, float3 N, float3 V)
{
    float3 T1 = V - N * dot(V, N);
    float lengthT1 = length(T1);
    T1 = lengthT1 > 1e-5f ? T1 / lengthT1 : (abs(N.z) < 0.999f ? normalize(cross(N, float3(0.0f, 0.0f, 1.0f))) : float3(1.0f, 0.0f, 0.0f));
    float3 T2 = cross(N, T1);
    float3x3 minv = MatrixFromColumns(float3(texel.x, 0.0f, texel.y), float3(0.0f, 1.0f, 0.0f), float3(texel.z, 0.0f, texel.w));
    return mul(minv, MatrixFromRows(T1, T2, N));
}

#endif
