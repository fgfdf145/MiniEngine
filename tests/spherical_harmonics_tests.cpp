#include <engine/renderer/spherical_harmonics.h>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
constexpr float kPi = 3.14159265358979f;

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

FloatTextureData MakeMap(int width, int height)
{
    FloatTextureData map{};
    map.width = width;
    map.height = height;
    map.pixels.assign(static_cast<size_t>(width) * height * 4, 0.0f);
    return map;
}

void SetTexel(FloatTextureData& map, int x, int y, const glm::vec3& value)
{
    float* texel = &map.pixels[(static_cast<size_t>(y) * map.width + x) * 4];
    texel[0] = value.r;
    texel[1] = value.g;
    texel[2] = value.b;
    texel[3] = 1.0f;
}

// The basis is orthonormal over the sphere: integrating Y_i Y_j gives the identity.
void BasisIsOrthonormal()
{
    constexpr int kSteps = 512;
    double gram[9][9] = {};
    for (int j = 0; j < kSteps; ++j)
    {
        for (int i = 0; i < kSteps * 2; ++i)
        {
            const float u = (static_cast<float>(i) + 0.5f) / (kSteps * 2);
            const float v = (static_cast<float>(j) + 0.5f) / kSteps;
            const glm::vec3 direction = EquirectangularDirection(u, v);
            const double weight = (2.0 * kPi / (kSteps * 2)) * (kPi / kSteps) * std::sin(v * kPi);
            const std::array<float, 9> basis = EvaluateShBasis(direction);
            for (int a = 0; a < 9; ++a)
            {
                for (int b = 0; b < 9; ++b)
                {
                    gram[a][b] += basis[a] * basis[b] * weight;
                }
            }
        }
    }
    for (int a = 0; a < 9; ++a)
    {
        for (int b = 0; b < 9; ++b)
        {
            const double expected = a == b ? 1.0 : 0.0;
            Require(std::fabs(gram[a][b] - expected) < 2e-3,
                    "basis inner product (" + std::to_string(a) + ", " + std::to_string(b) + ") is " + std::to_string(gram[a][b]));
        }
    }
}

void DirectionMatchesTheSampler()
{
    // SampleEnvironmentMap: u = 0.5 at -Z, 0.75 at +X; v = 0 straight up.
    Require(glm::length(EquirectangularDirection(0.5f, 0.5f) - glm::vec3(0.0f, 0.0f, -1.0f)) < 1e-5f, "u = 0.5 is -Z");
    Require(glm::length(EquirectangularDirection(0.75f, 0.5f) - glm::vec3(1.0f, 0.0f, 0.0f)) < 1e-5f, "u = 0.75 is +X");
    Require(glm::length(EquirectangularDirection(0.3f, 0.0f) - glm::vec3(0.0f, 1.0f, 0.0f)) < 1e-5f, "v = 0 is +Y");
}

void ConstantSkyGivesPiL()
{
    FloatTextureData map = MakeMap(64, 32);
    for (int y = 0; y < 32; ++y)
    {
        for (int x = 0; x < 64; ++x)
        {
            SetTexel(map, x, y, glm::vec3(1.0f, 2.0f, 3.0f));
        }
    }
    const ShCoefficients sh = ProjectEquirectangular(map);
    for (const glm::vec3& normal : {glm::vec3(0, 1, 0), glm::vec3(0, -1, 0), glm::vec3(1, 0, 0), glm::normalize(glm::vec3(1, 1, -1))})
    {
        const glm::vec3 irradiance = EvaluateShIrradiance(sh, normal);
        Require(glm::length(irradiance - kPi * glm::vec3(1.0f, 2.0f, 3.0f)) < 0.02f * kPi * 3.0f,
                "a constant sky gives pi L, got " + std::to_string(irradiance.r) + ", " + std::to_string(irradiance.g) + ", " + std::to_string(irradiance.b));
    }
}

void UpperHemisphereLightsUpwardFaces()
{
    FloatTextureData map = MakeMap(64, 32);
    for (int y = 0; y < 16; ++y)
    {
        for (int x = 0; x < 64; ++x)
        {
            SetTexel(map, x, y, glm::vec3(1.0f));
        }
    }
    const ShCoefficients sh = ProjectEquirectangular(map);
    const float up = EvaluateShIrradiance(sh, glm::vec3(0, 1, 0)).r;
    const float down = EvaluateShIrradiance(sh, glm::vec3(0, -1, 0)).r;
    const float side = EvaluateShIrradiance(sh, glm::vec3(1, 0, 0)).r;
    // Exact values are pi, 0 and pi / 2; L2 is within about 10% of them.
    Require(std::fabs(up - kPi) < 0.1f * kPi, "an upward face sees pi, got " + std::to_string(up));
    Require(std::fabs(down) < 0.1f * kPi, "a downward face sees nothing, got " + std::to_string(down));
    Require(std::fabs(side - 0.5f * kPi) < 0.05f * kPi, "a side face sees half, got " + std::to_string(side));
}

// Rotating the map by a quarter turn in SampleEnvironmentMap's sense (u sampled + 0.25) is the same
// as projecting a map whose columns are shifted by a quarter of its width.
void RotationMatchesShiftedMap()
{
    constexpr int kWidth = 64;
    constexpr int kHeight = 32;
    FloatTextureData map = MakeMap(kWidth, kHeight);
    for (int y = 0; y < kHeight; ++y)
    {
        for (int x = 0; x < kWidth; ++x)
        {
            const float u = (static_cast<float>(x) + 0.5f) / kWidth;
            const float v = (static_cast<float>(y) + 0.5f) / kHeight;
            SetTexel(map, x, y, glm::vec3(1.0f + std::cos(2.0f * kPi * u) * std::sin(kPi * v), 1.0f + std::sin(4.0f * kPi * u), 1.0f - v));
        }
    }
    FloatTextureData shifted = MakeMap(kWidth, kHeight);
    for (int y = 0; y < kHeight; ++y)
    {
        for (int x = 0; x < kWidth; ++x)
        {
            const float* source = &map.pixels[(static_cast<size_t>(y) * kWidth + (x + kWidth / 4) % kWidth) * 4];
            SetTexel(shifted, x, y, glm::vec3(source[0], source[1], source[2]));
        }
    }
    const ShCoefficients rotated = ShForHdriRotation(ProjectEquirectangular(map), 90.0f);
    const ShCoefficients expected = ProjectEquirectangular(shifted);
    for (int index = 0; index < 9; ++index)
    {
        Require(glm::length(rotated[index] - expected[index]) < 1e-3f,
                "coefficient " + std::to_string(index) + " of the rotated SH differs from the shifted map's");
    }
    Require(glm::length(ShForHdriRotation(ProjectEquirectangular(map), 0.0f)[4] - ProjectEquirectangular(map)[4]) < 1e-6f,
            "a zero rotation changes nothing");
}
}

int main()
{
    try
    {
        BasisIsOrthonormal();
        DirectionMatchesTheSampler();
        ConstantSkyGivesPiL();
        UpperHemisphereLightsUpwardFaces();
        RotationMatchesShiftedMap();
    }
    catch (const std::exception& error)
    {
        std::cerr << "spherical harmonics tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "spherical harmonics tests passed\n";
    return 0;
}
