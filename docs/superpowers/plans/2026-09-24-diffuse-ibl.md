# Diffuse IBL Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the constant ambient term with the sky's SH9 irradiance in Atmosphere and HDRI modes.

**Architecture:** A pure C++ SH module projects HDRIs (at load, on the worker) and rotates them per frame into the UBO; a compute dispatch in `VulkanAtmosphere` projects the atmosphere into a storage buffer each frame; `ShadeSurface` evaluates irradiance for N (diffuse) and R (interim specular).

**Tech Stack:** C++20, GLSL, Vulkan, CTest.

**Spec:** `docs/superpowers/specs/2026-09-24-diffuse-ibl-design.md`

## Global Constraints

- Commit to `main`; never stage `miniengine.settings.json` or `docs/superpowers/plans/2026-09-13-gbuffer-phase2-deferred-shading.md`.
- Build `cmake --build --preset vs2026-x64-debug --parallel`; configure `cmake --preset vs2026-x64` after adding files; tests `ctest --test-dir out/build/vs2026-x64 -C Debug --output-on-failure`; format check after `git add`: `powershell -ExecutionPolicy Bypass -File scripts/check-format.ps1`.
- SH basis, index order and constants exactly as spec decision 2, identical in C++ and GLSL.
- None mode must render byte-identically to before (compare with `p3before_cubes_none.png` in the scratchpad captures).
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

---

### Task 1: Spherical harmonics module

**Files:** Create `engine/renderer/spherical_harmonics.{h,cpp}`, `tests/spherical_harmonics_tests.cpp`; modify `engine/renderer/CMakeLists.txt` (engine_render_core), `tests/CMakeLists.txt`.

**Produces:** `me::ShCoefficients` (`std::array<glm::vec3, 9>`), `EvaluateShBasis(const glm::vec3&) -> std::array<float, 9>`, `EquirectangularDirection(float u, float v) -> glm::vec3`, `ProjectEquirectangular(const FloatTextureData&) -> ShCoefficients`, `RotateShAboutY(const ShCoefficients&, float radians)`, `ShForHdriRotation(const ShCoefficients&, float rotationDegrees)`, `EvaluateShIrradiance(const ShCoefficients&, const glm::vec3&) -> glm::vec3`.

- [ ] **Step 1: Failing test** `tests/spherical_harmonics_tests.cpp`:

```cpp
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
```

Register as `miniengine.spherical_harmonics` (target `miniengine_spherical_harmonics_tests`, links `engine_render_core`) after the atmosphere test block.

- [ ] **Step 2: Run to verify it fails** (header missing).

- [ ] **Step 3: Implement.** `engine/renderer/spherical_harmonics.h`:

```cpp
#pragma once

#include <engine/asset/texture_loader.h>

#include <glm/glm.hpp>

#include <array>

namespace me
{

// RGB radiance projected onto the nine real spherical harmonics up to band 2, in the Y-up basis of
// shaders/vulkan/spherical_harmonics.glsl (same order and constants): 0 Y00, 1 Y1-1 (z), 2 Y10 (y),
// 3 Y11 (x), 4 Y2-2 (xz), 5 Y2-1 (zy), 6 Y20 (3y^2 - 1), 7 Y21 (xy), 8 Y22 (x^2 - z^2).
using ShCoefficients = std::array<glm::vec3, 9>;

std::array<float, 9> EvaluateShBasis(const glm::vec3& direction);

// The world direction of equirectangular coordinates, the inverse of SampleEnvironmentMap in
// shaders/vulkan/atmosphere_sampling.glsl at zero rotation: u = 0.5 is -Z, 0.75 is +X, v = 0 is +Y.
glm::vec3 EquirectangularDirection(float u, float v);

// Projects every texel, weighted by its solid angle. Throws std::runtime_error for invalid data.
ShCoefficients ProjectEquirectangular(const FloatTextureData& map);

// The coefficients of f(R^-1 d), where R turns by radians about +Y (x toward -z).
ShCoefficients RotateShAboutY(const ShCoefficients& sh, float radians);

// The SH of a map drawn with HdriSettings::rotationDegrees, from the SH of the map itself.
ShCoefficients ShForHdriRotation(const ShCoefficients& sh, float rotationDegrees);

// Irradiance for a surface facing normal: the radiance convolved with the clamped cosine.
glm::vec3 EvaluateShIrradiance(const ShCoefficients& sh, const glm::vec3& normal);
}
```

`engine/renderer/spherical_harmonics.cpp`:

```cpp
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
    // SampleEnvironmentMap adds rotation / 360 to u, which turns the drawn map by -rotation about
    // +Y in the sense of RotateShAboutY.
    return RotateShAboutY(sh, -rotationDegrees * kPi / 180.0f);
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
```

- [ ] **Step 4: Run.** Configure, build, `ctest -R spherical_harmonics`. If only `RotationMatchesShiftedMap` fails, flip the sign in `ShForHdriRotation` and nothing else (that is the one convention this test pins); if the band 2 m = 2 pair alone disagrees, check the pair's derivation (x^2 - z^2 has half xz's constant, which is why it turns without extra factors). Commit `feat(renderer): spherical harmonics projection and rotation`.

---

### Task 2: HDRI irradiance on the CPU and the UBO block

**Files:** Modify `engine/renderer/atmosphere.{h,cpp}`, `tests/atmosphere_tests.cpp`, `shaders/vulkan/scene_common.glsl`, `engine/renderer/vulkan/uniform_buffer.{h,cpp}` (size asserts only), `engine/renderer/vulkan/renderer.{h,cpp}`.

**Produces:** `EnvironmentUniformData::hdriIrradianceSh[9]` (vec4, xyz used); `BuildEnvironmentUniformData(mode, environment, p, sun, cameraPositionMeters, const ShCoefficients* hdriSh)`; `ambientLuminance.w` = 1 when the ambient is the fallback.

- [ ] **Step 1: Test first.** In `BuildsUniformData` pass a last argument `nullptr` to the three existing calls, and add:

```cpp
    ShCoefficients sh{};
    sh[0] = glm::vec3(1.0f, 2.0f, 3.0f);
    sh[3] = glm::vec3(0.5f);
    SceneEnvironment hdri = environment;
    hdri.mode = EnvironmentMode::Hdri;
    hdri.hdri.intensity = 10.0f;
    hdri.hdri.rotationDegrees = 90.0f;
    const EnvironmentUniformData withSh =
        BuildEnvironmentUniformData(EnvironmentMode::Hdri, hdri, p, std::nullopt, glm::vec3(0.0f), &sh);
    const ShCoefficients expected = ShForHdriRotation(sh, 90.0f);
    for (int index = 0; index < 9; ++index)
    {
        Require(glm::length(glm::vec3(withSh.hdriIrradianceSh[index]) - expected[index] * 10.0f) < 1e-5f,
                "the HDRI SH is rotated and scaled by the intensity");
    }
```

(include `<engine/renderer/spherical_harmonics.h>` in the test). Run: fails to compile.

- [ ] **Step 2: Implement.** `atmosphere.h`: include `"spherical_harmonics.h"`; `EnvironmentUniformData` gains `glm::vec4 hdriIrradianceSh[9]{};` with comment `// The HDRI's radiance SH, rotated and scaled by its intensity; xyz used.`; the static_assert becomes `18 * 16`; `BuildEnvironmentUniformData` gains `const ShCoefficients* hdriSh` (null when no HDRI is loaded). In the .cpp, when `hdriSh` is non-null:

```cpp
    if (hdriSh != nullptr)
    {
        const ShCoefficients rotated = ShForHdriRotation(*hdriSh, environment.hdri.rotationDegrees);
        const float intensity = std::max(environment.hdri.intensity, 0.0f);
        for (size_t index = 0; index < rotated.size(); ++index)
        {
            data.hdriIrradianceSh[index] = glm::vec4(rotated[index] * intensity, 0.0f);
        }
    }
```

`scene_common.glsl`: after `hdriParameters`, `vec4 hdriIrradianceSh[9];         // the HDRI's radiance SH, rotated and scaled`. `uniform_buffer.h`: the `CameraUniformData` size assert `+ 9 * 16` becomes `+ 18 * 16`.

Renderer: the pending future becomes `std::future<PreparedEnvironmentMap>` with, in `renderer.h`,

```cpp
    // A decoded HDRI and its SH, prepared together on the worker thread.
    struct PreparedEnvironmentMap
    {
        FloatTextureData image;
        ShCoefficients sh{};
    };
```

the async lambda returns `PreparedEnvironmentMap{image, ProjectEquirectangular(image)}` after `LoadRGBA32F`, installation keeps `m_environmentMapSh = prepared.sh;` (new member `ShCoefficients m_environmentMapSh{};`), and `DrawFrame` passes `environmentMode == EnvironmentMode::Hdri ? &m_environmentMapSh : nullptr`. In `VulkanUniformBuffer::Update`, write `data.ambientLuminance = glm::vec4(ambientLuminance, usesFallbackAmbient ? 1.0f : 0.0f)` with a new `bool usesFallbackAmbient` parameter after `ambientLuminance`, passed `lightSelection.usesFallbackAmbient`; update the `scene_common.glsl` comment on `ambientLuminance` (`w = 1 when it is the fallback`).

- [ ] **Step 3: Verify.** Build, ctest (atmosphere passes), validation run; captures of `cubes_none` unchanged (nothing reads the new data yet). Commit `feat(renderer): HDRI irradiance SH on the CPU`.

---

### Task 3: Atmosphere projection on the GPU and sky-lit shading

**Files:** Create `shaders/vulkan/spherical_harmonics.glsl`, `shaders/vulkan/atmosphere_irradiance.comp`; modify `shaders/vulkan/atmosphere_sampling.glsl`, `shaders/vulkan/pbr_common.glsl`, `engine/renderer/vulkan/atmosphere.{h,cpp}`, `engine/renderer/vulkan/uniform_buffer.{h,cpp}`, `engine/renderer/vulkan/renderer.cpp`, `engine/renderer/CMakeLists.txt`.

- [ ] **Step 1: GLSL SH.** `shaders/vulkan/spherical_harmonics.glsl`:

```glsl
// Real spherical harmonics to band 2 in the Y-up basis of engine/renderer/spherical_harmonics.h:
// same order, same constants.
#ifndef SPHERICAL_HARMONICS_GLSL
#define SPHERICAL_HARMONICS_GLSL

void EvaluateShBasis(vec3 d, out float basis[9])
{
    basis[0] = 0.282095;
    basis[1] = 0.488603 * d.z;
    basis[2] = 0.488603 * d.y;
    basis[3] = 0.488603 * d.x;
    basis[4] = 1.092548 * d.x * d.z;
    basis[5] = 1.092548 * d.z * d.y;
    basis[6] = 0.315392 * (3.0 * d.y * d.y - 1.0);
    basis[7] = 1.092548 * d.x * d.y;
    basis[8] = 0.546274 * (d.x * d.x - d.z * d.z);
}

// Ramamoorthi and Hanrahan's clamped-cosine factors per coefficient.
const float SH_COSINE_LOBE[9] = float[9](
    3.14159265, 2.09439510, 2.09439510, 2.09439510,
    0.78539816, 0.78539816, 0.78539816, 0.78539816, 0.78539816);

#endif
```

- [ ] **Step 2: Sky radiance for lighting.** Append to `atmosphere_sampling.glsl` before `#endif`:

```glsl
// Radiance that lights surfaces: the sky without the sun's disk (the sun is a light of its own),
// and below the horizon the ground lit by the transmitted sun, which the sky-view LUT leaves out.
vec3 SampleSkyForLighting(vec3 direction)
{
    vec3 camera = ubo.atmosphereCameraPositionKm.xyz;
    float viewHeight = length(camera);
    vec3 up = camera / viewHeight;
    vec3 sunDirection = ubo.sunDirectionAndMode.xyz;

    float lightViewCos = 1.0;
    vec3 side = cross(up, direction);
    if (dot(side, side) > 1e-10)
    {
        side = normalize(side);
        vec3 forward = normalize(cross(side, up));
        vec2 sunOnPlane = vec2(dot(sunDirection, forward), dot(sunDirection, side));
        float length2 = dot(sunOnPlane, sunOnPlane);
        lightViewCos = length2 > 1e-12 ? sunOnPlane.x * inversesqrt(length2) : 1.0;
    }
    bool intersectGround = RaySphereIntersectNearest(camera, direction, vec3(0.0), BottomRadius()) >= 0.0;
    vec2 uv = SkyViewLutParamsToUv(intersectGround, dot(direction, up), lightViewCos, viewHeight);
    vec3 luminance = textureLod(atmosphereSkyViewLut, uv, 0.0).rgb;
    if (intersectGround)
    {
        float cosSunZenith = dot(up, sunDirection);
        vec3 transmittance = SampleTransmittance(atmosphereTransmittanceLut, BottomRadius() + PLANET_RADIUS_OFFSET_KM, cosSunZenith);
        luminance += ubo.groundAlbedo.rgb / ATMOSPHERE_PI * ubo.sunIlluminance.rgb * transmittance * max(cosSunZenith, 0.0);
    }
    return luminance;
}
```

- [ ] **Step 3: Projection shader.** `shaders/vulkan/atmosphere_irradiance.comp`:

```glsl
#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"
#include "atmosphere_sampling.glsl"
#include "spherical_harmonics.glsl"

// One workgroup: 64 invocations, 128 directions each, of a 128 x 64 grid uniform in azimuth and
// cos zenith, so every direction stands for the same solid angle, 4 pi / 8192.
layout(local_size_x = 64) in;

layout(set = 1, binding = 6, std430) writeonly buffer SkyIrradianceOut
{
    vec4 coefficients[9];
}
irradianceOut;

shared vec3 sharedSums[64][9];

void main()
{
    uint index = gl_LocalInvocationIndex;
    vec3 sums[9];
    for (int i = 0; i < 9; ++i)
    {
        sums[i] = vec3(0.0);
    }
    for (uint sampleIndex = index; sampleIndex < 8192u; sampleIndex += 64u)
    {
        float u = (float(sampleIndex % 128u) + 0.5) / 128.0;
        float v = (float(sampleIndex / 128u) + 0.5) / 64.0;
        float phi = 2.0 * ATMOSPHERE_PI * u;
        float cosTheta = 1.0 - 2.0 * v;
        float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
        vec3 direction = vec3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));
        vec3 radiance = SampleSkyForLighting(direction);
        float basis[9];
        EvaluateShBasis(direction, basis);
        for (int i = 0; i < 9; ++i)
        {
            sums[i] += radiance * basis[i];
        }
    }
    for (int i = 0; i < 9; ++i)
    {
        sharedSums[index][i] = sums[i];
    }
    barrier();
    for (uint stride = 32u; stride > 0u; stride >>= 1u)
    {
        if (index < stride)
        {
            for (int i = 0; i < 9; ++i)
            {
                sharedSums[index][i] += sharedSums[index + stride][i];
            }
        }
        barrier();
    }
    if (index < 9u)
    {
        irradianceOut.coefficients[index] = vec4(sharedSums[0][index] * (4.0 * ATMOSPHERE_PI / 8192.0), 0.0);
    }
}
```

Add it to the shader sources; `spherical_harmonics.glsl` to the includes.

- [ ] **Step 4: `VulkanAtmosphere`.** The compute set layout gains binding 6, `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` (pool: one storage buffer); a device-local buffer `m_irradianceBuffer` of `9 * 16` bytes (usage STORAGE | TRANSFER_DST), memory from `FindMemoryType(..., DEVICE_LOCAL)`; first `Record` zero-fills it with `vkCmdFillBuffer` next to the image clears; a fifth pipeline `atmosphere_irradiance.comp.spv` (`kShaderNames`/`m_pipelines` become 5 entries, with the `Lut` enum gaining `kIrradiance` after `kAerialPerspective` and a separate `kPipelineCount = 5` so image arrays stay 4). After the sky-view dispatch, a compute-to-compute barrier, then `Dispatch(commandBuffer, kIrradiance, 1, 1, 1)` (sky-view must finish first; the AP dispatch can stay before the barrier). `VkBuffer GetIrradianceBuffer() const`. Destroy the buffer and memory in `DestroyHandles`.

- [ ] **Step 5: Set 0.** `VulkanFrameDescriptorSetLayout`: bindings 3 to 6 stage `FRAGMENT | COMPUTE`; binding 7 `STORAGE_BUFFER`, fragment. `EnvironmentDescriptorBindings` gains `VkBuffer irradiance = VK_NULL_HANDLE;` written to binding 7 (range `VK_WHOLE_SIZE`); pool storage buffers `imageCount * 2`; frame writes 8. `BuildEnvironmentBindings` sets `irradiance = m_atmosphere->GetIrradianceBuffer()`. `atmosphere_sampling.glsl` declares

```glsl
layout(set = 0, binding = 7, std430) readonly buffer SkyIrradiance
{
    vec4 coefficients[9];
}
skyIrradiance;
```

- [ ] **Step 6: Shading.** `pbr_common.glsl`: include `"atmosphere_sampling.glsl"` and `"spherical_harmonics.glsl"` at the top if the file does not already include `scene_common.glsl` users in a way that makes this circular (it is included after `scene_common.glsl` everywhere; add the includes there). Add before `ShadeSurface`:

```glsl
// Sky irradiance for a direction from the active sky's SH: the atmosphere's (computed on the GPU)
// or the HDRI's (projected on the CPU, in the camera block).
vec3 EvaluateSkyIrradiance(vec3 direction)
{
    float basis[9];
    EvaluateShBasis(direction, basis);
    bool hdri = EnvironmentMode() == ENVIRONMENT_HDRI;
    vec3 irradiance = vec3(0.0);
    for (int i = 0; i < 9; ++i)
    {
        vec3 coefficient = hdri ? ubo.hdriIrradianceSh[i].rgb : skyIrradiance.coefficients[i].rgb;
        irradiance += coefficient * (SH_COSINE_LOBE[i] * basis[i]);
    }
    return max(irradiance, vec3(0.0));
}

// The ambient term under a physical sky: the diffuse lobe sees the irradiance for N, the specular
// lobe the cosine-blurred radiance along R (phase 4 prefilters it properly), each divided by pi to
// turn irradiance into the radiance of a Lambertian reflector. The scene's Ambient lights, but not
// the fallback, add their uniform luminance.
vec3 EvaluateSkyAmbient(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness)
{
    float NdV = max(dot(N, V), 0.0);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec2 environmentBrdf = EnvironmentBrdfApprox(roughness, NdV);
    vec3 specularAlbedo = F0 * environmentBrdf.x + environmentBrdf.y;
    vec3 diffuseAlbedo = albedo * (1.0 - metallic) * (vec3(1.0) - specularAlbedo);
    vec3 R = reflect(-V, N);
    vec3 sky = (diffuseAlbedo * EvaluateSkyIrradiance(N) + specularAlbedo * EvaluateSkyIrradiance(R)) / ATMOSPHERE_PI;
    vec3 sceneAmbient = ubo.ambientLuminance.w > 0.5 ? vec3(0.0) : ubo.ambientLuminance.rgb;
    return sky + (diffuseAlbedo + specularAlbedo) * sceneAmbient;
}
```

and in `ShadeSurface`:

```glsl
    vec3 ambient = EnvironmentMode() == ENVIRONMENT_NONE
                       ? EvaluateUniformAmbient(N, V, albedo, metallic, roughness, ubo.ambientLuminance.rgb)
                       : EvaluateSkyAmbient(N, V, albedo, metallic, roughness);
    ambient *= ao;
```

Then remove the now-duplicate `#include "atmosphere_sampling.glsl"` lines from `deferred_lighting.frag` and `triangle.frag` only if the include guard would not already make them harmless (it does; leave them). Check `gbuffer.frag` and `shadow.frag` do not include `pbr_common.glsl` (if `gbuffer.frag` does, its pipeline layout also uses set 0 and is fine).

- [ ] **Step 7: Verify.** Build, ctest, validation runs (startup, Sponza, the `cubes_*` scenes). Captures:
  - `cubes_none`: byte-identical to `p3before_cubes_none.png` (decode both with the scratchpad `png.py` and compare pixels).
  - `cubes_hdri`: front faces (+Z) white-grey, the right cube's visible left face (-X) blue, the left cube's visible right face (+X) green.
  - `cubes_noon` and the startup scene: faces the sun misses get bluish sky light instead of black.
  - Sponza under the atmosphere: interior no longer black.
  Commit `feat(renderer): diffuse sky lighting from SH irradiance`.

---

### Task 4: Measurement and docs

- [ ] Throwaway timestamps around the irradiance dispatch (as phase 2 did; discard afterwards).
- [ ] README paragraph after the sky paragraph; spec amendments for anything that changed.
- [ ] Commit `docs: diffuse IBL`; report to the user with captures and the acceptance checklist.
