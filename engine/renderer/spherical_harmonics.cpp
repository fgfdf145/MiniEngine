#include "spherical_harmonics.h"

#include <cmath>
#include <stdexcept>

namespace me
{

namespace
{
constexpr float kPi = 3.14159265358979f;
// Ramamoorthi and Hanrahan's clamped-cosine convolution factors per band.
constexpr std::array<float, 9> kCosineLobe = {
    kPi,
    2.0f * kPi / 3.0f, 2.0f * kPi / 3.0f, 2.0f * kPi / 3.0f,
    kPi / 4.0f, kPi / 4.0f, kPi / 4.0f, kPi / 4.0f, kPi / 4.0f};
}

std::array<float, 9> EvaluateShBasis(const glm::vec3& d)
{
    return {
        0.282095f,
        0.488603f * d.z,
        0.488603f * d.y,
        0.488603f * d.x,
        1.092548f * d.x * d.z,
        1.092548f * d.z * d.y,
        0.315392f * (3.0f * d.y * d.y - 1.0f),
        1.092548f * d.x * d.y,
        0.546274f * (d.x * d.x - d.z * d.z)};
}

glm::vec3 EquirectangularDirection(float u, float v)
{
    const float theta = v * kPi;
    const float phi = (u - 0.5f) * 2.0f * kPi;
    return glm::vec3(std::sin(theta) * std::sin(phi), std::cos(theta), -std::sin(theta) * std::cos(phi));
}

ShCoefficients ProjectEquirectangular(const FloatTextureData& map)
{
    if (!map.IsValid())
    {
        throw std::runtime_error("Cannot project an invalid environment map");
    }
    // Accumulated in double: a 4K map sums millions of small terms.
    std::array<glm::dvec3, 9> sum{};
    const double texelWidth = 2.0 * kPi / map.width;
    const double texelHeight = kPi / map.height;
    for (int y = 0; y < map.height; ++y)
    {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(map.height);
        const double weight = texelWidth * texelHeight * std::sin(v * kPi);
        for (int x = 0; x < map.width; ++x)
        {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(map.width);
            const float* texel = &map.pixels[(static_cast<size_t>(y) * map.width + x) * 4];
            const glm::dvec3 radiance(texel[0], texel[1], texel[2]);
            const std::array<float, 9> basis = EvaluateShBasis(EquirectangularDirection(u, v));
            for (size_t index = 0; index < 9; ++index)
            {
                sum[index] += radiance * (basis[index] * weight);
            }
        }
    }
    ShCoefficients sh{};
    for (size_t index = 0; index < 9; ++index)
    {
        sh[index] = glm::vec3(sum[index]);
    }
    return sh;
}

ShCoefficients RotateShAboutY(const ShCoefficients& sh, float radians)
{
    // Band 1 and band 2's m = 1 pair are (z-like, x-like) components that turn by the angle; band
    // 2's m = 2 pair (xz, x^2 - z^2) turns by twice it. The m = 0 terms do not change.
    const float c1 = std::cos(radians);
    const float s1 = std::sin(radians);
    const float c2 = std::cos(2.0f * radians);
    const float s2 = std::sin(2.0f * radians);
    ShCoefficients out = sh;
    out[3] = sh[3] * c1 + sh[1] * s1;
    out[1] = sh[1] * c1 - sh[3] * s1;
    out[7] = sh[7] * c1 + sh[5] * s1;
    out[5] = sh[5] * c1 - sh[7] * s1;
    out[4] = sh[4] * c2 - sh[8] * s2;
    out[8] = sh[4] * s2 + sh[8] * c2;
    return out;
}

ShCoefficients ShForHdriRotation(const ShCoefficients& sh, float rotationDegrees)
{
    // SampleEnvironmentMap adds rotation / 360 to u, which turns the drawn map by +rotation about
    // +Y in the sense of RotateShAboutY (pinned by RotationMatchesShiftedMap).
    return RotateShAboutY(sh, rotationDegrees * kPi / 180.0f);
}

glm::vec3 EvaluateShIrradiance(const ShCoefficients& sh, const glm::vec3& normal)
{
    const std::array<float, 9> basis = EvaluateShBasis(normal);
    glm::vec3 irradiance(0.0f);
    for (size_t index = 0; index < 9; ++index)
    {
        irradiance += sh[index] * (kCosineLobe[index] * basis[index]);
    }
    return glm::max(irradiance, glm::vec3(0.0f));
}
}
