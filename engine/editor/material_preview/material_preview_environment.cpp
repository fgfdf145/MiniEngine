#include "material_preview_environment.h"

#include <engine/asset/texture_loader.h>
#include <engine/core/threading/task_system.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

namespace me
{

namespace
{
constexpr float kPi = 3.14159265358979f;

// The prefiltered levels, as the probe's mips: roughness 0, 0.2, ... 1.
constexpr int kLevelCount = 6;
constexpr int kLevelWidths[kLevelCount] = {512, 256, 128, 64, 64, 32};
constexpr uint32_t kPrefilterSamples = 512;

float SmoothStep(float edge0, float edge1, float x)
{
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// A soft-edged rectangle: 1 inside half-extents (a, b), falling to 0 over `soft` beyond them.
float SoftRectangle(float x, float y, float a, float b, float soft)
{
    return (1.0f - SmoothStep(a, a + soft, std::abs(x))) * (1.0f - SmoothStep(b, b + soft, std::abs(y)));
}

glm::vec3 StudioRadiance(const glm::vec3& direction)
{
    const float y = direction.y;
    // The room: a dim ceiling, darker walls toward the floor, a dark floor.
    glm::vec3 radiance = y >= 0.0f ? glm::mix(glm::vec3(0.09f), glm::vec3(0.16f), SmoothStep(0.0f, 0.8f, y))
                                   : glm::mix(glm::vec3(0.09f), glm::vec3(0.035f), SmoothStep(0.0f, 0.3f, -y));
    // The softbox overhead, a rectangle on the ceiling plane y = 1, longer along z.
    if (y > 0.05f)
    {
        const float x = direction.x / y;
        const float z = direction.z / y;
        radiance += glm::vec3(1.6f) * SoftRectangle(x, z, 0.55f, 1.1f, 0.25f);
    }
    // Two strip lights either side, tall and narrow, on the planes x = +-1.
    if (std::abs(direction.x) > 0.05f)
    {
        const float z = direction.z / std::abs(direction.x);
        const float height = direction.y / std::abs(direction.x);
        radiance += glm::vec3(0.95f, 0.97f, 1.0f) * SoftRectangle(z + 0.15f, height - 0.35f, 0.12f, 0.55f, 0.08f);
    }
    // A wide, dim panel behind, for a rim along silhouettes.
    if (direction.z > 0.05f)
    {
        const float x = direction.x / direction.z;
        const float height = direction.y / direction.z;
        radiance += glm::vec3(0.35f) * SoftRectangle(x, height - 0.25f, 1.2f, 0.3f, 0.3f);
    }
    return radiance;
}

constexpr glm::vec3 kDaylightSun{0.45f, 0.62f, -0.64f};

glm::vec3 DaylightRadiance(const glm::vec3& direction)
{
    const float y = direction.y;
    if (y < 0.0f)
    {
        // Sunlit ground, a little brighter toward the horizon where it meets the haze.
        return glm::mix(glm::vec3(0.16f, 0.13f, 0.10f), glm::vec3(0.30f, 0.30f, 0.30f), std::pow(1.0f + y, 8.0f));
    }
    const glm::vec3 zenith(0.10f, 0.22f, 0.55f);
    const glm::vec3 horizon(0.55f, 0.66f, 0.80f);
    glm::vec3 radiance = glm::mix(zenith, horizon, std::pow(1.0f - y, 3.0f));
    const float toSun = std::max(glm::dot(direction, glm::normalize(kDaylightSun)), 0.0f);
    radiance += glm::vec3(1.0f, 0.85f, 0.6f) * (0.6f * std::pow(toSun, 64.0f) + 0.15f * std::pow(toSun, 6.0f));
    return radiance;
}

glm::vec3 PresetRadiance(MaterialPreviewEnvironmentPreset preset, const glm::vec3& direction)
{
    return preset == MaterialPreviewEnvironmentPreset::Daylight ? DaylightRadiance(direction) : StudioRadiance(direction);
}

float RadicalInverse(uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

MaterialPreviewEnvironment::Level MakeLevel(int width)
{
    MaterialPreviewEnvironment::Level level;
    level.width = width;
    level.height = std::max(width / 2, 1);
    level.texels.resize(static_cast<size_t>(level.width) * level.height);
    return level;
}
}

const char* ToString(MaterialPreviewEnvironmentPreset preset)
{
    switch (preset)
    {
    case MaterialPreviewEnvironmentPreset::Daylight:
        return "Daylight";
    case MaterialPreviewEnvironmentPreset::Studio:
    default:
        return "Studio";
    }
}

glm::vec2 MaterialPreviewEquirectangularUv(const glm::vec3& direction)
{
    const float theta = std::acos(std::clamp(direction.y, -1.0f, 1.0f));
    const float phi = std::atan2(direction.x, -direction.z);
    return glm::vec2(phi / (2.0f * kPi) + 0.5f, theta / kPi);
}

glm::vec3 MaterialPreviewEnvironment::Level::Sample(const glm::vec3& direction) const
{
    const glm::vec2 uv = MaterialPreviewEquirectangularUv(direction);
    const float x = uv.x * static_cast<float>(width) - 0.5f;
    const float y = std::clamp(uv.y * static_cast<float>(height) - 0.5f, 0.0f, static_cast<float>(height - 1));
    const float floorX = std::floor(x);
    const float floorY = std::floor(y);
    const float tx = x - floorX;
    const float ty = y - floorY;
    const auto wrapX = [this](int value)
    {
        const int wrapped = value % width;
        return wrapped < 0 ? wrapped + width : wrapped;
    };
    const int x0 = wrapX(static_cast<int>(floorX));
    const int x1 = wrapX(static_cast<int>(floorX) + 1);
    const int y0 = static_cast<int>(floorY);
    const int y1 = std::min(y0 + 1, height - 1);
    const glm::vec3* const data = texels.data();
    const glm::vec3 top = glm::mix(data[static_cast<size_t>(y0) * width + x0], data[static_cast<size_t>(y0) * width + x1], tx);
    const glm::vec3 bottom = glm::mix(data[static_cast<size_t>(y1) * width + x0], data[static_cast<size_t>(y1) * width + x1], tx);
    return glm::mix(top, bottom, ty);
}

std::shared_ptr<const MaterialPreviewEnvironment> MaterialPreviewEnvironment::Get(MaterialPreviewEnvironmentPreset preset)
{
    static std::mutex mutex;
    static std::array<std::shared_ptr<const MaterialPreviewEnvironment>, kMaterialPreviewEnvironmentPresetCount> built;
    const uint32_t index = std::min(static_cast<uint32_t>(preset), kMaterialPreviewEnvironmentPresetCount - 1);
    const std::lock_guard lock(mutex);
    if (!built[index])
    {
        built[index] = Build(static_cast<MaterialPreviewEnvironmentPreset>(index));
    }
    return built[index];
}

std::shared_ptr<const MaterialPreviewEnvironment> MaterialPreviewEnvironment::Build(MaterialPreviewEnvironmentPreset preset)
{
    auto environment = std::make_shared<MaterialPreviewEnvironment>();
    environment->m_preset = preset;
    if (preset == MaterialPreviewEnvironmentPreset::Daylight)
    {
        environment->m_keyDirection = glm::normalize(kDaylightSun);
        environment->m_keyIlluminance = glm::vec3(1.0f, 0.95f, 0.88f) * 3.2f;
        environment->m_keyAngularRadius = 0.0093f;
    }
    else
    {
        // From above, in front and to the side, as a photographer's key.
        environment->m_keyDirection = glm::normalize(glm::vec3(-0.55f, 0.75f, -0.45f));
        environment->m_keyIlluminance = glm::vec3(1.0f, 0.98f, 0.95f) * 1.4f;
        environment->m_keyAngularRadius = 0.06f;
    }

    // Level 0: the sky itself, 2 x 2 samples a texel so the softboxes' edges do not alias.
    environment->m_levels.push_back(MakeLevel(kLevelWidths[0]));
    {
        Level& level = environment->m_levels.front();
        TaskSystem::ParallelFor(
            static_cast<uint32_t>(level.height),
            4,
            [&](uint32_t begin, uint32_t end)
            {
                for (uint32_t y = begin; y < end; ++y)
                {
                    for (int x = 0; x < level.width; ++x)
                    {
                        glm::vec3 sum(0.0f);
                        for (int sample = 0; sample < 4; ++sample)
                        {
                            const float u = (static_cast<float>(x) + 0.25f + 0.5f * static_cast<float>(sample & 1)) / static_cast<float>(level.width);
                            const float v = (static_cast<float>(y) + 0.25f + 0.5f * static_cast<float>(sample >> 1)) / static_cast<float>(level.height);
                            sum += PresetRadiance(preset, EquirectangularDirection(u, v));
                        }
                        level.texels[static_cast<size_t>(y) * level.width + x] = sum * 0.25f;
                    }
                }
            },
            TaskPriority::Medium);
    }

    // The GGX-prefiltered levels (Karis 2013, N = V = R), importance sampled from level 0.
    for (int levelIndex = 1; levelIndex < kLevelCount; ++levelIndex)
    {
        Level level = MakeLevel(kLevelWidths[levelIndex]);
        const float roughness = static_cast<float>(levelIndex) / static_cast<float>(kLevelCount - 1);
        const float alpha = roughness * roughness;
        const Level& source = environment->m_levels.front();
        TaskSystem::ParallelFor(
            static_cast<uint32_t>(level.height),
            1,
            [&](uint32_t begin, uint32_t end)
            {
                for (uint32_t y = begin; y < end; ++y)
                {
                    for (int x = 0; x < level.width; ++x)
                    {
                        const glm::vec3 normal = EquirectangularDirection(
                            (static_cast<float>(x) + 0.5f) / static_cast<float>(level.width),
                            (static_cast<float>(y) + 0.5f) / static_cast<float>(level.height));
                        const glm::vec3 up = std::abs(normal.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
                        const glm::vec3 tangent = glm::normalize(glm::cross(up, normal));
                        const glm::vec3 bitangent = glm::cross(normal, tangent);
                        glm::vec3 sum(0.0f);
                        float weight = 0.0f;
                        for (uint32_t sample = 0; sample < kPrefilterSamples; ++sample)
                        {
                            const float e1 = (static_cast<float>(sample) + 0.5f) / static_cast<float>(kPrefilterSamples);
                            const float e2 = RadicalInverse(sample);
                            const float phi = 2.0f * kPi * e2;
                            const float cosTheta = std::sqrt((1.0f - e1) / (1.0f + (alpha * alpha - 1.0f) * e1));
                            const float sinTheta = std::sqrt(std::max(1.0f - cosTheta * cosTheta, 0.0f));
                            const glm::vec3 half =
                                tangent * (sinTheta * std::cos(phi)) + bitangent * (sinTheta * std::sin(phi)) + normal * cosTheta;
                            const glm::vec3 light = 2.0f * glm::dot(normal, half) * half - normal;
                            const float nDotL = glm::dot(normal, light);
                            if (nDotL > 0.0f)
                            {
                                sum += source.Sample(light) * nDotL;
                                weight += nDotL;
                            }
                        }
                        level.texels[static_cast<size_t>(y) * level.width + x] = weight > 0.0f ? sum / weight : glm::vec3(0.0f);
                    }
                }
            },
            TaskPriority::Medium);
        environment->m_levels.push_back(std::move(level));
    }

    FloatTextureData map;
    const Level& sky = environment->m_levels.front();
    map.width = sky.width;
    map.height = sky.height;
    map.pixels.resize(static_cast<size_t>(sky.width) * sky.height * 4);
    for (size_t index = 0; index < sky.texels.size(); ++index)
    {
        map.pixels[index * 4 + 0] = sky.texels[index].r;
        map.pixels[index * 4 + 1] = sky.texels[index].g;
        map.pixels[index * 4 + 2] = sky.texels[index].b;
        map.pixels[index * 4 + 3] = 1.0f;
    }
    environment->m_sh = ProjectEquirectangular(map);
    return environment;
}

glm::vec3 MaterialPreviewEnvironment::Specular(const glm::vec3& direction, float roughness) const
{
    const float level = std::clamp(roughness, 0.0f, 1.0f) * static_cast<float>(m_levels.size() - 1);
    const int lower = static_cast<int>(std::floor(level));
    const float blend = level - static_cast<float>(lower);
    const Level* const levels = m_levels.data();
    const glm::vec3 a = levels[lower].Sample(direction);
    if (blend <= 0.0f || lower + 1 >= static_cast<int>(m_levels.size()))
    {
        return a;
    }
    return glm::mix(a, levels[lower + 1].Sample(direction), blend);
}

glm::vec3 MaterialPreviewEnvironment::Background(const glm::vec3& direction) const
{
    return Specular(direction, 0.12f);
}

glm::vec3 MaterialPreviewEnvironment::DiffuseRadiance(const glm::vec3& normal) const
{
    return EvaluateShIrradiance(m_sh, normal) * (1.0f / kPi);
}
}
