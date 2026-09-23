# Specular IBL Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split-sum specular IBL from the active sky: a GGX-prefiltered cubemap plus a DFG lookup table.

**Architecture:** A pure C++ function integrates the DFG table, uploaded once. `VulkanEnvironmentProbe` captures the sky into a radiance cube each frame, blits its mip chain, and prefilters a second cube with GGX importance sampling; `EvaluateSkyAmbient` samples both.

**Tech Stack:** C++20, GLSL compute, Vulkan, CTest.

**Spec:** `docs/superpowers/specs/2026-09-24-specular-ibl-design.md`

## Global Constraints

- Commit to `main`; never stage `miniengine.settings.json` or `docs/superpowers/plans/2026-09-13-gbuffer-phase2-deferred-shading.md`.
- Build/configure/test/format commands as in the phase 3 plan; `git add` new files before the format check.
- Cube sizes: radiance 128, 8 mips; prefiltered 128, 6 mips (roughness = mip / 5); DFG table 64 x 64, 512 samples. Set 0 binding 8 prefiltered cube, binding 9 DFG table.
- None mode unchanged (within run-to-run noise: EV within 0.05, pixels within 2).
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

---

### Task 1: Environment BRDF table

**Files:** Create `engine/renderer/environment_brdf.{h,cpp}`, `tests/environment_brdf_tests.cpp`; modify `engine/renderer/CMakeLists.txt` (engine_render_core), `tests/CMakeLists.txt`.

- [ ] **Step 1: Failing test** `tests/environment_brdf_tests.cpp`:

```cpp
#include <engine/renderer/environment_brdf.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// Karis' analytic fit of the same table, as pbr_common.glsl's EnvironmentBrdfApprox has it.
glm::vec2 KarisFit(float roughness, float NdV)
{
    const glm::vec4 c0(-1.0f, -0.0275f, -0.572f, 0.022f);
    const glm::vec4 c1(1.0f, 0.0425f, 1.04f, -0.04f);
    const glm::vec4 r = roughness * c0 + c1;
    const float a004 = std::min(r.x * r.x, std::exp2(-9.28f * NdV)) * r.x + r.y;
    return glm::vec2(-1.04f, 1.04f) * a004 + glm::vec2(r.z, r.w);
}

void MirrorHeadOnReflectsEverything()
{
    const glm::vec2 ab = IntegrateEnvironmentBrdf(0.0f, 1.0f, 512);
    Require(std::fabs(ab.x - 1.0f) < 1e-3f && std::fabs(ab.y) < 1e-3f,
            "a mirror seen head on has A = 1, B = 0, got " + std::to_string(ab.x) + ", " + std::to_string(ab.y));
}

void MatchesKarisFit()
{
    for (float roughness = 0.1f; roughness <= 1.0f; roughness += 0.1f)
    {
        for (float NdV = 0.1f; NdV <= 1.0f; NdV += 0.1f)
        {
            const glm::vec2 ab = IntegrateEnvironmentBrdf(roughness, NdV, 512);
            const glm::vec2 fit = KarisFit(roughness, NdV);
            Require(ab.x + ab.y <= 1.0f + 1e-3f, "the table never reflects more than it receives");
            Require(std::fabs(ab.x - fit.x) < 0.06f && std::fabs(ab.y - fit.y) < 0.06f,
                    "roughness " + std::to_string(roughness) + ", N.V " + std::to_string(NdV) + ": (" + std::to_string(ab.x) + ", " +
                        std::to_string(ab.y) + ") against the fit's (" + std::to_string(fit.x) + ", " + std::to_string(fit.y) + ")");
        }
    }
}

void TableSamplesTexelCentres()
{
    const FloatTextureData table = BuildEnvironmentBrdfLut(8, 64);
    Require(table.IsValid() && table.width == 8 && table.height == 8, "the table is size x size");
    // Texel (x, y): N.V = (x + 0.5) / 8 across, roughness = (y + 0.5) / 8 down.
    const glm::vec2 expected = IntegrateEnvironmentBrdf(5.5f / 8.0f, 2.5f / 8.0f, 64);
    const float* texel = &table.pixels[(5 * 8 + 2) * 4];
    Require(std::fabs(texel[0] - expected.x) < 1e-6f && std::fabs(texel[1] - expected.y) < 1e-6f && texel[3] == 1.0f,
            "texel (2, 5) holds roughness 5.5 / 8 at N.V 2.5 / 8");
}
}

int main()
{
    try
    {
        MirrorHeadOnReflectsEverything();
        MatchesKarisFit();
        TableSamplesTexelCentres();
    }
    catch (const std::exception& error)
    {
        std::cerr << "environment BRDF tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "environment BRDF tests passed\n";
    return 0;
}
```

Register `miniengine.environment_brdf` (target `miniengine_environment_brdf_tests`, links `engine_render_core`) after the spherical harmonics block.

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Implement.** `engine/renderer/environment_brdf.h`:

```cpp
#pragma once

#include <engine/asset/texture_loader.h>

#include <glm/glm.hpp>

#include <cstdint>

namespace me
{

// The DFG table set 0 binding 9 samples: 64 x 64, 512 samples per texel.
inline constexpr uint32_t kEnvironmentBrdfLutSize = 64;
inline constexpr uint32_t kEnvironmentBrdfSampleCount = 512;

// The split-sum environment BRDF (Karis 2013): specular albedo = F0 * A + B for a GGX lobe of
// this roughness seen at this N.V, integrated with sampleCount GGX importance samples and
// Schlick-Smith visibility with k = alpha / 2 (alpha = roughness^2), the remapping Karis uses for
// IBL.
glm::vec2 IntegrateEnvironmentBrdf(float roughness, float NdV, uint32_t sampleCount);

// The table: texel (x, y) holds (A, B, 0, 1) for N.V = (x + 0.5) / size and roughness =
// (y + 0.5) / size, rows top-down like every texture.
FloatTextureData BuildEnvironmentBrdfLut(uint32_t size, uint32_t sampleCount);
}
```

`engine/renderer/environment_brdf.cpp`:

```cpp
#include "environment_brdf.h"

#include <algorithm>
#include <cmath>

namespace me
{

namespace
{
constexpr float kPi = 3.14159265358979f;

float RadicalInverse(uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

// A GGX half vector in tangent space (N = +Z).
glm::vec3 ImportanceSampleGgx(float u, float v, float roughness)
{
    const float alpha = roughness * roughness;
    const float phi = 2.0f * kPi * u;
    const float cosTheta = std::sqrt((1.0f - v) / (1.0f + (alpha * alpha - 1.0f) * v));
    const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
    return glm::vec3(sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta);
}

float SchlickSmithG1(float NdX, float k)
{
    return NdX / (NdX * (1.0f - k) + k);
}
}

glm::vec2 IntegrateEnvironmentBrdf(float roughness, float NdV, uint32_t sampleCount)
{
    NdV = std::clamp(NdV, 1e-4f, 1.0f);
    const glm::vec3 V(std::sqrt(1.0f - NdV * NdV), 0.0f, NdV);
    const float k = roughness * roughness / 2.0f;
    float a = 0.0f;
    float b = 0.0f;
    for (uint32_t i = 0; i < sampleCount; ++i)
    {
        const glm::vec3 H = ImportanceSampleGgx(static_cast<float>(i) / static_cast<float>(sampleCount), RadicalInverse(i), roughness);
        const glm::vec3 L = 2.0f * glm::dot(V, H) * H - V;
        const float NdL = std::clamp(L.z, 0.0f, 1.0f);
        const float NdH = std::clamp(H.z, 0.0f, 1.0f);
        const float VdH = std::clamp(glm::dot(V, H), 0.0f, 1.0f);
        if (NdL > 0.0f && NdH > 0.0f)
        {
            const float G = SchlickSmithG1(NdV, k) * SchlickSmithG1(NdL, k);
            // pdf = D NdH / (4 VdH); dividing the BRDF times NdL by it leaves G VdH / (NdH NdV).
            const float visibility = G * VdH / (NdH * NdV);
            const float fresnel = std::pow(1.0f - VdH, 5.0f);
            a += (1.0f - fresnel) * visibility;
            b += fresnel * visibility;
        }
    }
    return glm::vec2(a, b) / static_cast<float>(sampleCount);
}

FloatTextureData BuildEnvironmentBrdfLut(uint32_t size, uint32_t sampleCount)
{
    FloatTextureData table{};
    table.width = static_cast<int>(size);
    table.height = static_cast<int>(size);
    table.pixels.resize(static_cast<size_t>(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y)
    {
        const float roughness = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
        for (uint32_t x = 0; x < size; ++x)
        {
            const float NdV = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
            const glm::vec2 ab = IntegrateEnvironmentBrdf(roughness, NdV, sampleCount);
            float* texel = &table.pixels[(static_cast<size_t>(y) * size + x) * 4];
            texel[0] = ab.x;
            texel[1] = ab.y;
            texel[2] = 0.0f;
            texel[3] = 1.0f;
        }
    }
    return table;
}
}
```

- [ ] **Step 4: Run to verify it passes.** If `MatchesKarisFit` misses by a little at the grazing corner (N.V 0.1, high roughness), print the grid first: the fit is known to be loosest there; widen the tolerance only for N.V below 0.2, and say so in the test. Commit `feat(renderer): environment BRDF table`.

---

### Task 2: Environment probe and split-sum shading

**Files:** Create `shaders/vulkan/cubemap_common.glsl`, `shaders/vulkan/environment_capture.comp`, `shaders/vulkan/environment_prefilter.comp`, `engine/renderer/vulkan/environment_probe.{h,cpp}`; modify `shaders/vulkan/atmosphere_sampling.glsl`, `shaders/vulkan/pbr_common.glsl`, `engine/renderer/vulkan/uniform_buffer.{h,cpp}`, `engine/renderer/vulkan/renderer.{h,cpp}`, `engine/renderer/CMakeLists.txt`.

- [ ] **Step 1: GLSL.** `shaders/vulkan/cubemap_common.glsl`:

```glsl
// Cube map directions and the probe's sizes, shared by the capture and prefilter shaders and the
// shading that samples the prefiltered cube.
#ifndef CUBEMAP_COMMON_GLSL
#define CUBEMAP_COMMON_GLSL

// Must match engine/renderer/vulkan/environment_probe.cpp.
const float RADIANCE_CUBE_SIZE = 128.0;
const float RADIANCE_CUBE_MIP_COUNT = 8.0;
const float PREFILTER_MIP_COUNT = 6.0;

// Vulkan's cube face orientation: face 0..5 = +X, -X, +Y, -Y, +Z, -Z; uv with t growing down.
vec3 CubeFaceDirection(uint face, vec2 uv)
{
    vec2 st = uv * 2.0 - 1.0;
    float s = st.x;
    float t = st.y;
    vec3 direction;
    if (face == 0u)
    {
        direction = vec3(1.0, -t, -s);
    }
    else if (face == 1u)
    {
        direction = vec3(-1.0, -t, s);
    }
    else if (face == 2u)
    {
        direction = vec3(s, 1.0, t);
    }
    else if (face == 3u)
    {
        direction = vec3(s, -1.0, -t);
    }
    else if (face == 4u)
    {
        direction = vec3(s, -t, 1.0);
    }
    else
    {
        direction = vec3(-s, -t, -1.0);
    }
    return normalize(direction);
}

#endif
```

`shaders/vulkan/environment_capture.comp`:

```glsl
#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"
#include "atmosphere_sampling.glsl"
#include "cubemap_common.glsl"

// The sky's radiance into mip 0 of the radiance cube: 2 x 2 samples per texel, no sun disk.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 1, binding = 0, rgba16f) uniform writeonly image2DArray radianceOut;

void main()
{
    uvec3 id = gl_GlobalInvocationID;
    if (id.x >= uint(RADIANCE_CUBE_SIZE) || id.y >= uint(RADIANCE_CUBE_SIZE))
    {
        return;
    }
    bool hdri = EnvironmentMode() == ENVIRONMENT_HDRI;
    vec3 sum = vec3(0.0);
    for (uint sy = 0u; sy < 2u; ++sy)
    {
        for (uint sx = 0u; sx < 2u; ++sx)
        {
            vec2 uv = (vec2(id.xy) + (vec2(sx, sy) + 0.5) * 0.5) / RADIANCE_CUBE_SIZE;
            vec3 direction = CubeFaceDirection(id.z, uv);
            sum += hdri ? SampleEnvironmentMap(direction) : SampleSkyForLighting(direction);
        }
    }
    imageStore(radianceOut, ivec3(id), vec4(min(sum * 0.25, vec3(65504.0)), 1.0));
}
```

`shaders/vulkan/environment_prefilter.comp`:

```glsl
#version 450
#extension GL_GOOGLE_include_directive : require

#include "cubemap_common.glsl"

// One mip of the prefiltered cube: GGX importance sampling with N = V = R (Karis 2013), each
// sample read from the radiance cube at the mip whose texel solid angle matches its own.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 1, binding = 1) uniform samplerCube radianceCube;
layout(set = 1, binding = 2, rgba16f) uniform writeonly image2DArray prefilteredOut;

// Must match PrefilterConstants in engine/renderer/vulkan/environment_probe.cpp.
layout(push_constant) uniform PrefilterConstants
{
    float roughness;
    uint size;
}
constants;

const float PI = 3.14159265358979;
const uint SAMPLE_COUNT = 64u;

float RadicalInverse(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec3 ImportanceSampleGgx(vec2 xi, float roughness, vec3 N)
{
    float alpha = roughness * roughness;
    float phi = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    vec3 H = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangentX = normalize(cross(up, N));
    vec3 tangentY = cross(N, tangentX);
    return tangentX * H.x + tangentY * H.y + N * H.z;
}

void main()
{
    uvec3 id = gl_GlobalInvocationID;
    if (id.x >= constants.size || id.y >= constants.size)
    {
        return;
    }
    vec3 N = CubeFaceDirection(id.z, (vec2(id.xy) + 0.5) / float(constants.size));
    vec3 color;
    if (constants.roughness <= 0.0)
    {
        color = textureLod(radianceCube, N, 0.0).rgb;
    }
    else
    {
        float alpha = constants.roughness * constants.roughness;
        float texelSolidAngle = 4.0 * PI / (6.0 * RADIANCE_CUBE_SIZE * RADIANCE_CUBE_SIZE);
        vec3 sum = vec3(0.0);
        float weight = 0.0;
        for (uint i = 0u; i < SAMPLE_COUNT; ++i)
        {
            vec3 H = ImportanceSampleGgx(vec2(float(i) / float(SAMPLE_COUNT), RadicalInverse(i)), constants.roughness, N);
            vec3 L = normalize(2.0 * dot(N, H) * H - N);
            float NdL = dot(N, L);
            if (NdL > 0.0)
            {
                float NdH = max(dot(N, H), 0.0);
                float denominator = NdH * NdH * (alpha * alpha - 1.0) + 1.0;
                float D = alpha * alpha / (PI * denominator * denominator);
                // With V = N, VdH = NdH and the pdf D NdH / (4 VdH) is D / 4.
                float pdf = D * 0.25;
                float sampleSolidAngle = 1.0 / (float(SAMPLE_COUNT) * pdf + 1e-4);
                float lod = clamp(0.5 * log2(sampleSolidAngle / texelSolidAngle), 0.0, RADIANCE_CUBE_MIP_COUNT - 1.0);
                sum += textureLod(radianceCube, L, lod).rgb * NdL;
                weight += NdL;
            }
        }
        color = sum / max(weight, 1e-4);
    }
    imageStore(prefilteredOut, ivec3(id), vec4(color, 1.0));
}
```

`atmosphere_sampling.glsl`, after the binding 7 declaration:

```glsl
// Set 0 bindings 8 and 9: the GGX-prefiltered sky (mip m for roughness m / 5) and the DFG table
// (x = N.V, y = roughness).
layout(set = 0, binding = 8) uniform samplerCube prefilteredEnvironment;
layout(set = 0, binding = 9) uniform sampler2D environmentBrdfLut;
```

Add `environment_capture.comp`, `environment_prefilter.comp` to the shader sources and `cubemap_common.glsl` to the includes.

- [ ] **Step 2: Shading.** In `pbr_common.glsl`, include `"cubemap_common.glsl"` next to the other includes, and replace `EvaluateSkyAmbient`:

```glsl
// The DFG table at (roughness, N.V), clamped to texel centres so the lookup never wraps.
vec2 SampleEnvironmentBrdf(float roughness, float NdV)
{
    const float size = 64.0;
    vec2 uv = clamp(vec2(NdV, roughness), vec2(0.5 / size), vec2(1.0 - 0.5 / size));
    return textureLod(environmentBrdfLut, uv, 0.0).rg;
}

// The ambient term under a physical sky, split-sum (Karis 2013): the diffuse lobe sees the SH
// irradiance for N, the specular lobe the GGX-prefiltered sky along R at the surface's roughness,
// weighted by the DFG table's F0 A + B. The scene's Ambient lights, but not the fallback, add their
// uniform luminance.
vec3 EvaluateSkyAmbient(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness)
{
    float NdV = max(dot(N, V), 0.0);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec2 environmentBrdf = SampleEnvironmentBrdf(roughness, NdV);
    vec3 specularAlbedo = F0 * environmentBrdf.x + environmentBrdf.y;
    vec3 diffuseAlbedo = albedo * (1.0 - metallic) * (vec3(1.0) - specularAlbedo);
    vec3 R = reflect(-V, N);
    vec3 specular = textureLod(prefilteredEnvironment, R, roughness * (PREFILTER_MIP_COUNT - 1.0)).rgb;
    vec3 sky = diffuseAlbedo * EvaluateSkyIrradiance(N) / ATMOSPHERE_PI + specularAlbedo * specular;
    vec3 sceneAmbient = ubo.ambientLuminance.w > 0.5 ? vec3(0.0) : ubo.ambientLuminance.rgb;
    return sky + (diffuseAlbedo + specularAlbedo) * sceneAmbient;
}
```

- [ ] **Step 3: `VulkanEnvironmentProbe`.** `engine/renderer/vulkan/environment_probe.h`:

```cpp
#pragma once

#include "common.h"
#include "uniform_buffer.h"

#include <array>

namespace me
{

// The sky as the specular lobe sees it: each frame under a physical sky, the sky's radiance is
// captured into a 128 x 128 cube (the radiance cube), its mip chain blitted, and a second cube
// prefiltered with GGX for six roughnesses (mip m for roughness m / 5). Like VulkanAtmosphere it is
// device-lifetime, shared by the frames in flight, kept in VK_IMAGE_LAYOUT_GENERAL, and orders
// itself with its own barriers; it records after VulkanAtmosphere, whose sky-view LUT the capture
// samples. Set 0 binding 8 names the prefiltered cube for every draw, so the first Record clears
// both cubes whatever the mode.
class VulkanEnvironmentProbe
{
  public:
    VulkanEnvironmentProbe(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkPipelineCache pipelineCache,
        VkDescriptorSetLayout frameSetLayout);
    ~VulkanEnvironmentProbe();

    VulkanEnvironmentProbe(const VulkanEnvironmentProbe&) = delete;
    VulkanEnvironmentProbe& operator=(const VulkanEnvironmentProbe&) = delete;

    // physicalSky is false in EnvironmentMode::None, when nothing is captured.
    void Record(VkCommandBuffer commandBuffer, VkDescriptorSet frameDescriptorSet, bool physicalSky);

    TextureDescriptorBinding GetPrefilteredBinding() const;

  private:
    static constexpr uint32_t kCubeSize = 128;
    static constexpr uint32_t kRadianceMipCount = 8;
    static constexpr uint32_t kPrefilterMipCount = 6;

    struct CubeImage
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView cubeView = VK_NULL_HANDLE;
    };

    CubeImage CreateCube(uint32_t mipCount, VkImageUsageFlags usage) const;
    VkImageView CreateArrayView(VkImage image, uint32_t mip) const;
    void CreateDescriptors();
    void CreatePipelines(VkPipelineCache pipelineCache, VkDescriptorSetLayout frameSetLayout);
    void RecordMipChain(VkCommandBuffer commandBuffer) const;
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
    void DestroyHandles();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    CubeImage m_radiance;
    CubeImage m_prefiltered;
    VkImageView m_radianceStorageView = VK_NULL_HANDLE;
    std::array<VkImageView, kPrefilterMipCount> m_prefilteredStorageViews{};
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    // One per prefiltered mip; set 0 also serves the capture.
    std::array<VkDescriptorSet, kPrefilterMipCount> m_descriptorSets{};
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_capturePipeline = VK_NULL_HANDLE;
    VkPipeline m_prefilterPipeline = VK_NULL_HANDLE;
    bool m_imagesInitialized = false;
};
}
```

`engine/renderer/vulkan/environment_probe.cpp`:

```cpp
#include "environment_probe.h"

#include "pipeline.h"

#include <engine/core/paths/engine_paths.h>

#include <stdexcept>

namespace me
{

namespace
{
constexpr VkFormat kCubeFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// Must match PrefilterConstants in shaders/vulkan/environment_prefilter.comp.
struct PrefilterConstants
{
    float roughness = 0.0f;
    uint32_t size = 0;
};

void GlobalBarrier(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags srcStage,
    VkAccessFlags srcAccess,
    VkPipelineStageFlags dstStage,
    VkAccessFlags dstAccess)
{
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

uint32_t GroupCount(uint32_t size)
{
    return (size + 7) / 8;
}
}

VulkanEnvironmentProbe::VulkanEnvironmentProbe(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkPipelineCache pipelineCache,
    VkDescriptorSetLayout frameSetLayout)
    : m_physicalDevice(physicalDevice),
      m_device(device)
{
    try
    {
        m_radiance = CreateCube(
            kRadianceMipCount,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        m_prefiltered = CreateCube(
            kPrefilterMipCount,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        m_radianceStorageView = CreateArrayView(m_radiance.image, 0);
        for (uint32_t mip = 0; mip < kPrefilterMipCount; ++mip)
        {
            m_prefilteredStorageViews[mip] = CreateArrayView(m_prefiltered.image, mip);
        }

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        samplerInfo.maxLod = static_cast<float>(kRadianceMipCount);
        samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        CheckVulkan(vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler), "Failed to create the environment probe sampler");

        CreateDescriptors();
        CreatePipelines(pipelineCache, frameSetLayout);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanEnvironmentProbe::~VulkanEnvironmentProbe()
{
    DestroyHandles();
}

TextureDescriptorBinding VulkanEnvironmentProbe::GetPrefilteredBinding() const
{
    return TextureDescriptorBinding{m_prefiltered.cubeView, m_sampler};
}

void VulkanEnvironmentProbe::Record(VkCommandBuffer commandBuffer, VkDescriptorSet frameDescriptorSet, bool physicalSky)
{
    if (!m_imagesInitialized)
    {
        std::array<VkImageMemoryBarrier, 2> barriers{};
        const std::array<std::pair<VkImage, uint32_t>, 2> images = {
            std::pair{m_radiance.image, kRadianceMipCount},
            std::pair{m_prefiltered.image, kPrefilterMipCount}};
        for (size_t index = 0; index < barriers.size(); ++index)
        {
            VkImageMemoryBarrier& barrier = barriers[index];
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = images[index].first;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, images[index].second, 0, 6};
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        vkCmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
            static_cast<uint32_t>(barriers.size()), barriers.data());
        const VkClearColorValue black{};
        for (const auto& [image, mipCount] : images)
        {
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, mipCount, 0, 6};
            vkCmdClearColorImage(commandBuffer, image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
        }
        m_imagesInitialized = true;
    }

    // The previous frame's reads and this frame's clears before this frame's writes.
    GlobalBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);

    if (physicalSky)
    {
        const std::array<VkDescriptorSet, 2> captureSets = {frameDescriptorSet, m_descriptorSets[0]};
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_capturePipeline);
        vkCmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 2, captureSets.data(), 0, nullptr);
        vkCmdDispatch(commandBuffer, GroupCount(kCubeSize), GroupCount(kCubeSize), 6);

        GlobalBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        RecordMipChain(commandBuffer);
        GlobalBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_prefilterPipeline);
        for (uint32_t mip = 0; mip < kPrefilterMipCount; ++mip)
        {
            const std::array<VkDescriptorSet, 2> sets = {frameDescriptorSet, m_descriptorSets[mip]};
            vkCmdBindDescriptorSets(
                commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout, 0, 2, sets.data(), 0, nullptr);
            PrefilterConstants constants{};
            constants.roughness = static_cast<float>(mip) / static_cast<float>(kPrefilterMipCount - 1);
            constants.size = kCubeSize >> mip;
            vkCmdPushConstants(commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
            vkCmdDispatch(commandBuffer, GroupCount(constants.size), GroupCount(constants.size), 6);
        }
    }

    // This frame's writes before its fragment shaders sample them.
    GlobalBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
}

void VulkanEnvironmentProbe::RecordMipChain(VkCommandBuffer commandBuffer) const
{
    for (uint32_t mip = 1; mip < kRadianceMipCount; ++mip)
    {
        const int32_t sourceSize = static_cast<int32_t>(kCubeSize >> (mip - 1));
        const int32_t targetSize = static_cast<int32_t>(kCubeSize >> mip);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 6};
        blit.srcOffsets[1] = {sourceSize, sourceSize, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 6};
        blit.dstOffsets[1] = {targetSize, targetSize, 1};
        vkCmdBlitImage(
            commandBuffer, m_radiance.image, VK_IMAGE_LAYOUT_GENERAL, m_radiance.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
        // Each level reads the one written just before it.
        GlobalBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    }
}

VulkanEnvironmentProbe::CubeImage VulkanEnvironmentProbe::CreateCube(uint32_t mipCount, VkImageUsageFlags usage) const
{
    CubeImage cube{};
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {kCubeSize, kCubeSize, 1};
    imageInfo.mipLevels = mipCount;
    imageInfo.arrayLayers = 6;
    imageInfo.format = kCubeFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CheckVulkan(vkCreateImage(m_device, &imageInfo, nullptr, &cube.image), "Failed to create an environment cube");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(m_device, cube.image, &requirements);
    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &cube.memory), "Failed to allocate an environment cube");
    CheckVulkan(vkBindImageMemory(m_device, cube.image, cube.memory, 0), "Failed to bind an environment cube");

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = cube.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = kCubeFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipCount, 0, 6};
    CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &cube.cubeView), "Failed to create an environment cube view");
    return cube;
}

VkImageView VulkanEnvironmentProbe::CreateArrayView(VkImage image, uint32_t mip) const
{
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = kCubeFormat;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 6};
    VkImageView view = VK_NULL_HANDLE;
    CheckVulkan(vkCreateImageView(m_device, &viewInfo, nullptr, &view), "Failed to create an environment cube storage view");
    return view;
}

void VulkanEnvironmentProbe::CreateDescriptors()
{
    // 0 the radiance cube's mip 0 (capture output), 1 the radiance cube sampled, 2 one prefiltered
    // mip (prefilter output).
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    for (uint32_t binding = 0; binding < bindings.size(); ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = binding == 1 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    CheckVulkan(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_setLayout), "Failed to create the environment probe set layout");

    const std::array<VkDescriptorPoolSize, 2> poolSizes = {
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kPrefilterMipCount},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kPrefilterMipCount}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kPrefilterMipCount;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "Failed to create the environment probe descriptor pool");

    const std::array<VkDescriptorSetLayout, kPrefilterMipCount> layouts = {
        m_setLayout, m_setLayout, m_setLayout, m_setLayout, m_setLayout, m_setLayout};
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = m_descriptorPool;
    allocateInfo.descriptorSetCount = kPrefilterMipCount;
    allocateInfo.pSetLayouts = layouts.data();
    CheckVulkan(vkAllocateDescriptorSets(m_device, &allocateInfo, m_descriptorSets.data()), "Failed to allocate the environment probe descriptor sets");

    const VkDescriptorImageInfo captureInfo{VK_NULL_HANDLE, m_radianceStorageView, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo radianceInfo{m_sampler, m_radiance.cubeView, VK_IMAGE_LAYOUT_GENERAL};
    for (uint32_t mip = 0; mip < kPrefilterMipCount; ++mip)
    {
        const VkDescriptorImageInfo prefilteredInfo{VK_NULL_HANDLE, m_prefilteredStorageViews[mip], VK_IMAGE_LAYOUT_GENERAL};
        const std::array<const VkDescriptorImageInfo*, 3> infos = {&captureInfo, &radianceInfo, &prefilteredInfo};
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (uint32_t binding = 0; binding < writes.size(); ++binding)
        {
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = m_descriptorSets[mip];
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType = bindings[binding].descriptorType;
            writes[binding].pImageInfo = infos[binding];
        }
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void VulkanEnvironmentProbe::CreatePipelines(VkPipelineCache pipelineCache, VkDescriptorSetLayout frameSetLayout)
{
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.size = sizeof(PrefilterConstants);
    const std::array<VkDescriptorSetLayout, 2> setLayouts = {frameSetLayout, m_setLayout};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    CheckVulkan(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout), "Failed to create the environment probe pipeline layout");

    const std::array<std::pair<const char*, VkPipeline*>, 2> pipelines = {
        std::pair{"environment_capture.comp.spv", &m_capturePipeline},
        std::pair{"environment_prefilter.comp.spv", &m_prefilterPipeline}};
    for (const auto& [shaderName, pipeline] : pipelines)
    {
        const VulkanShaderModule shader(m_device, EnginePaths::ShaderRoot() / shaderName);
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shader.GetHandle();
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = m_pipelineLayout;
        CheckVulkan(vkCreateComputePipelines(m_device, pipelineCache, 1, &pipelineInfo, nullptr, pipeline), "Failed to create an environment probe pipeline");
    }
}

uint32_t VulkanEnvironmentProbe::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);
    for (uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index)
    {
        if ((typeFilter & (1u << index)) != 0 && (memoryProperties.memoryTypes[index].propertyFlags & properties) == properties)
        {
            return index;
        }
    }
    throw std::runtime_error("Failed to find a memory type for the environment probe");
}

void VulkanEnvironmentProbe::DestroyHandles()
{
    for (VkPipeline* pipeline : {&m_capturePipeline, &m_prefilterPipeline})
    {
        if (*pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(m_device, *pipeline, nullptr);
            *pipeline = VK_NULL_HANDLE;
        }
    }
    if (m_pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
        m_descriptorSets = {};
    }
    if (m_setLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    for (VkImageView& view : m_prefilteredStorageViews)
    {
        if (view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, view, nullptr);
            view = VK_NULL_HANDLE;
        }
    }
    if (m_radianceStorageView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_device, m_radianceStorageView, nullptr);
        m_radianceStorageView = VK_NULL_HANDLE;
    }
    for (CubeImage* cube : {&m_radiance, &m_prefiltered})
    {
        if (cube->cubeView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(m_device, cube->cubeView, nullptr);
        }
        if (cube->image != VK_NULL_HANDLE)
        {
            vkDestroyImage(m_device, cube->image, nullptr);
        }
        if (cube->memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, cube->memory, nullptr);
        }
        *cube = CubeImage{};
    }
}
}
```

Add both files to `engine_renderer` after `vulkan/device.h`.

- [ ] **Step 4: Set 0 bindings 8 and 9.** `EnvironmentDescriptorBindings` gains `TextureDescriptorBinding prefiltered;` (layout `GENERAL`) and `TextureDescriptorBinding brdfLut;` (`SHADER_READ_ONLY_OPTIMAL`). `VulkanFrameDescriptorSetLayout`: ten bindings, 8 and 9 combined image samplers, fragment. Pool samplers `imageCount * 7`; frame writes 10.

- [ ] **Step 5: Renderer.** Members `std::unique_ptr<VulkanEnvironmentProbe> m_environmentProbe;` and `std::unique_ptr<VulkanTexture> m_environmentBrdfLut;`. In `CreateDeviceResources`, after `m_atmosphere`: construct the probe; in the same upload batch as the default environment map, `m_environmentBrdfLut = std::make_unique<VulkanTexture>(..., BuildEnvironmentBrdfLut(kEnvironmentBrdfLutSize, kEnvironmentBrdfSampleCount), uploadBatch)` (the equirectangular constructor: one mip, RGBA32F; the shader clamps to texel centres so the u repeat never shows). `DestroyDeviceResources` resets both before `m_atmosphere`. `BuildEnvironmentBindings` fills `prefiltered` and `brdfLut`. In the command buffer lambda, after `m_atmosphere->Record(...)`:

```cpp
                                              m_environmentProbe->Record(
                                                  commandBuffer,
                                                  frame.frameDescriptorSet,
                                                  environmentMode != EnvironmentMode::None);
```

- [ ] **Step 6: Verify.** Build, ctest, validation runs (startup, Sponza, `cubes_*`, `hdri_*`). Face-orientation check: temporarily make `sky.frag` return `textureLod(prefilteredEnvironment, direction, 0.0).rgb` for the HDRI branch and capture `hdri_0`, `hdri_90`, `hdri_r-90`, `hdri_r180`: the centre colours must be red, green, blue, white as with the direct lookup; then with lod 5 the image must be smooth. Revert. Captures of `cubes_noon`, `cubes_hdri`, `p3after` comparisons; None within noise. Commit `feat(renderer): split-sum specular IBL`.

---

### Task 3: Measurement and docs

- [ ] Throwaway timestamps around the probe's Record in Release; discard.
- [ ] README paragraph, spec amendments, memory; commit `docs: specular IBL`; report with the acceptance checklist.
