#pragma once

// Lets C++ compile the shader headers it shares with the GPU (exposure_histogram.slang, the BRDF and
// tone mapping headers the tests check, ...). Those headers keep to the subset of Slang that C++
// accepts once Slang's type names and the intrinsics GLM spells differently are defined: include
// them inside a namespace that has `using namespace glm;` and `using namespace me::shader_cpp;`.

#include <glm/glm.hpp>

namespace me::shader_cpp
{
using float2 = glm::vec2;
using float3 = glm::vec3;
using float4 = glm::vec4;
using int2 = glm::ivec2;
using int3 = glm::ivec3;
using int4 = glm::ivec4;
using uint2 = glm::uvec2;
using uint3 = glm::uvec3;
using uint4 = glm::uvec4;
using bool2 = glm::bvec2;
using bool3 = glm::bvec3;
using bool4 = glm::bvec4;
using float3x3 = glm::mat3;
using float4x4 = glm::mat4;

template <typename T, typename U>
T lerp(const T& a, const T& b, const U& t)
{
    return glm::mix(a, b, t);
}

inline float saturate(float x)
{
    return glm::clamp(x, 0.0f, 1.0f);
}
template <glm::length_t N>
glm::vec<N, float> saturate(const glm::vec<N, float>& x)
{
    return glm::clamp(x, 0.0f, 1.0f);
}

template <typename T>
T frac(const T& x)
{
    return glm::fract(x);
}

template <typename T>
T rsqrt(const T& x)
{
    return glm::inversesqrt(x);
}

template <typename T>
T atan2(const T& y, const T& x)
{
    return glm::atan(y, x);
}

inline unsigned int asuint(float x)
{
    return glm::floatBitsToUint(x);
}
inline float asfloat(unsigned int x)
{
    return glm::uintBitsToFloat(x);
}

// Slang's mul: matrix times column vector, row vector times matrix, matrix product.
template <glm::length_t C, glm::length_t R>
glm::vec<R, float> mul(const glm::mat<C, R, float>& m, const glm::vec<C, float>& v)
{
    return m * v;
}
template <glm::length_t C, glm::length_t R>
glm::vec<C, float> mul(const glm::vec<R, float>& v, const glm::mat<C, R, float>& m)
{
    return v * m;
}
template <glm::length_t C, glm::length_t R, glm::length_t K>
glm::mat<C, R, float> mul(const glm::mat<K, R, float>& a, const glm::mat<C, K, float>& b)
{
    return a * b;
}

// shader_helpers.slang's matrix builders: GLM's constructors take columns, Slang's rows.
inline glm::mat3 MatrixFromColumns(const glm::vec3& c0, const glm::vec3& c1, const glm::vec3& c2)
{
    return glm::mat3(c0, c1, c2);
}
inline glm::mat4 MatrixFromColumns(const glm::vec4& c0, const glm::vec4& c1, const glm::vec4& c2, const glm::vec4& c3)
{
    return glm::mat4(c0, c1, c2, c3);
}
inline glm::mat3 MatrixFromRows(const glm::vec3& r0, const glm::vec3& r1, const glm::vec3& r2)
{
    return glm::transpose(glm::mat3(r0, r1, r2));
}
}
