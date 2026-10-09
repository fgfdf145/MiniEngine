#include "material_preview_textures.h"

#include <engine/asset/texture_loader.h>
#include <engine/core/log/log.h>
#include <engine/core/threading/task_system.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>

namespace me
{

namespace
{
const std::array<float, 256>& SrgbTable()
{
    static const std::array<float, 256> table = []
    {
        std::array<float, 256> values{};
        for (int index = 0; index < 256; ++index)
        {
            const float encoded = static_cast<float>(index) / 255.0f;
            values[index] = encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
        }
        return values;
    }();
    return table;
}

// One level halved by a 2x2 box (odd sizes clamp to the edge).
MaterialPreviewTexture::Level Halve(const MaterialPreviewTexture::Level& source)
{
    MaterialPreviewTexture::Level level;
    level.width = std::max(source.width / 2, 1);
    level.height = std::max(source.height / 2, 1);
    level.rgba.resize(static_cast<size_t>(level.width) * level.height * 4);
    const uint8_t* const in = source.rgba.data();
    uint8_t* const out = level.rgba.data();
    for (int y = 0; y < level.height; ++y)
    {
        const int y0 = std::min(2 * y, source.height - 1);
        const int y1 = std::min(2 * y + 1, source.height - 1);
        for (int x = 0; x < level.width; ++x)
        {
            const int x0 = std::min(2 * x, source.width - 1);
            const int x1 = std::min(2 * x + 1, source.width - 1);
            const uint8_t* a = in + (static_cast<size_t>(y0) * source.width + x0) * 4;
            const uint8_t* b = in + (static_cast<size_t>(y0) * source.width + x1) * 4;
            const uint8_t* c = in + (static_cast<size_t>(y1) * source.width + x0) * 4;
            const uint8_t* d = in + (static_cast<size_t>(y1) * source.width + x1) * 4;
            uint8_t* o = out + (static_cast<size_t>(y) * level.width + x) * 4;
            for (int channel = 0; channel < 4; ++channel)
            {
                o[channel] = static_cast<uint8_t>((a[channel] + b[channel] + c[channel] + d[channel] + 2) / 4);
            }
        }
    }
    return level;
}

int WrapTexel(int index, int size, TextureWrap wrap)
{
    switch (wrap)
    {
    case TextureWrap::ClampToEdge:
        return std::clamp(index, 0, size - 1);
    case TextureWrap::MirroredRepeat:
    {
        const int period = 2 * size;
        int wrapped = index % period;
        if (wrapped < 0)
        {
            wrapped += period;
        }
        return wrapped < size ? wrapped : period - 1 - wrapped;
    }
    case TextureWrap::Repeat:
    default:
    {
        const int wrapped = index % size;
        return wrapped < 0 ? wrapped + size : wrapped;
    }
    }
}

glm::vec4 FetchTexel(const MaterialPreviewTexture::Level& level, int x, int y, bool srgb)
{
    const uint8_t* texel = level.rgba.data() + (static_cast<size_t>(y) * level.width + x) * 4;
    if (srgb)
    {
        const float* const table = SrgbTable().data();
        return glm::vec4(table[texel[0]], table[texel[1]], table[texel[2]], static_cast<float>(texel[3]) * (1.0f / 255.0f));
    }
    constexpr float kScale = 1.0f / 255.0f;
    return glm::vec4(texel[0] * kScale, texel[1] * kScale, texel[2] * kScale, texel[3] * kScale);
}

glm::vec4 SampleLevel(const MaterialPreviewTexture::Level& level, glm::vec2 uv, const TextureSampler& sampler, bool nearest, bool srgb)
{
    const float x = uv.x * static_cast<float>(level.width);
    const float y = uv.y * static_cast<float>(level.height);
    if (nearest)
    {
        const int ix = WrapTexel(static_cast<int>(std::floor(x)), level.width, sampler.wrapS);
        const int iy = WrapTexel(static_cast<int>(std::floor(y)), level.height, sampler.wrapT);
        return FetchTexel(level, ix, iy, srgb);
    }
    const float fx = x - 0.5f;
    const float fy = y - 0.5f;
    const float floorX = std::floor(fx);
    const float floorY = std::floor(fy);
    const float tx = fx - floorX;
    const float ty = fy - floorY;
    const int x0 = WrapTexel(static_cast<int>(floorX), level.width, sampler.wrapS);
    const int x1 = WrapTexel(static_cast<int>(floorX) + 1, level.width, sampler.wrapS);
    const int y0 = WrapTexel(static_cast<int>(floorY), level.height, sampler.wrapT);
    const int y1 = WrapTexel(static_cast<int>(floorY) + 1, level.height, sampler.wrapT);
    const glm::vec4 top = glm::mix(FetchTexel(level, x0, y0, srgb), FetchTexel(level, x1, y0, srgb), tx);
    const glm::vec4 bottom = glm::mix(FetchTexel(level, x0, y1, srgb), FetchTexel(level, x1, y1, srgb), tx);
    return glm::mix(top, bottom, ty);
}
}

float MaterialPreviewSrgbToLinear(uint8_t value)
{
    return SrgbTable()[value];
}

glm::vec4 MaterialPreviewTexture::Sample(glm::vec2 uv, float lod, const TextureSampler& sampler, bool srgb, float dither) const
{
    if (levels.empty() || !std::isfinite(uv.x) || !std::isfinite(uv.y))
    {
        return glm::vec4(1.0f);
    }
    const float maxLevel = static_cast<float>(levels.size() - 1);
    float level = sampler.mipFilter == TextureMipFilter::None ? 0.0f : std::clamp(std::isfinite(lod) ? lod : 0.0f, 0.0f, maxLevel);
    const bool magnified = level <= 0.0f;
    const bool nearest = magnified ? sampler.magFilter == TextureFilter::Nearest : sampler.minFilter == TextureFilter::Nearest;
    if (sampler.mipFilter == TextureMipFilter::Nearest)
    {
        level = std::round(level);
    }
    else if (dither >= 0.0f && level > 0.0f)
    {
        level = std::min(std::floor(level + dither), maxLevel);
    }
    const int lower = static_cast<int>(std::floor(level));
    const float blend = level - static_cast<float>(lower);
    const Level* const data = levels.data();
    const glm::vec4 a = SampleLevel(data[lower], uv, sampler, nearest, srgb);
    if (blend <= 0.0f || lower + 1 >= static_cast<int>(levels.size()))
    {
        return a;
    }
    const glm::vec4 b = SampleLevel(data[lower + 1], uv, sampler, nearest, srgb);
    return glm::mix(a, b, blend);
}

MaterialPreviewTexture BuildMaterialPreviewTexture(int width, int height, const uint8_t* rgba)
{
    MaterialPreviewTexture texture;
    if (width <= 0 || height <= 0 || rgba == nullptr)
    {
        return texture;
    }
    MaterialPreviewTexture::Level base;
    base.width = width;
    base.height = height;
    base.rgba.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);
    while (base.width > kMaterialPreviewMaxTextureSize || base.height > kMaterialPreviewMaxTextureSize)
    {
        base = Halve(base);
    }
    texture.levels.push_back(std::move(base));
    while (texture.levels.back().width > 1 || texture.levels.back().height > 1)
    {
        texture.levels.push_back(Halve(texture.levels.back()));
    }
    return texture;
}

MaterialPreviewTextureCache::~MaterialPreviewTextureCache()
{
    if (m_batch && TaskSystem::IsRunning())
    {
        TaskSystem::Scheduler().WaitforTask(m_batch.get());
    }
}

std::shared_ptr<const MaterialPreviewTexture> MaterialPreviewTextureCache::Find(const std::string& path) const
{
    const auto found = m_textures.find(path);
    return found == m_textures.end() ? nullptr : found->second;
}

void MaterialPreviewTextureCache::Request(const std::vector<std::string>& paths)
{
    for (const std::string& path : paths)
    {
        if (path.empty() || m_textures.contains(path) || m_failed.contains(path) || m_inFlight.contains(path))
        {
            continue;
        }
        m_inFlight.insert(path);
        m_queued.push_back(path);
    }
    if (!m_batch)
    {
        StartBatch();
    }
}

void MaterialPreviewTextureCache::Batch::ExecuteRange(enki::TaskSetPartition range, uint32_t threadNumber)
{
    static_cast<void>(threadNumber);
    for (uint32_t index = range.start; index < range.end; ++index)
    {
        try
        {
            const TextureData decoded = TextureLoader::LoadRGBA8(paths[index]);
            if (decoded.IsValid() && decoded.pixels.size() >= static_cast<size_t>(decoded.width) * decoded.height * 4)
            {
                results[index] = std::make_shared<const MaterialPreviewTexture>(
                    BuildMaterialPreviewTexture(decoded.width, decoded.height, decoded.pixels.data()));
            }
        }
        catch (const std::exception& error)
        {
            LOG_WARN("Material preview: {}", error.what());
        }
    }
}

void MaterialPreviewTextureCache::StartBatch()
{
    if (m_queued.empty())
    {
        return;
    }
    m_batch = std::make_unique<Batch>();
    m_batch->paths = std::move(m_queued);
    m_queued.clear();
    m_batch->results.resize(m_batch->paths.size());
    m_batch->m_SetSize = static_cast<uint32_t>(m_batch->paths.size());
    m_batch->m_MinRange = 1;
    m_batch->m_Priority = enki::TASK_PRIORITY_LOW;
    if (TaskSystem::IsRunning() && TaskSystem::CanWaitOnCurrentThread())
    {
        TaskSystem::Scheduler().AddTaskSetToPipe(m_batch.get());
    }
    else
    {
        m_batch->ExecuteRange(enki::TaskSetPartition{0, m_batch->m_SetSize}, 0);
    }
}

bool MaterialPreviewTextureCache::Poll()
{
    if (!m_batch)
    {
        StartBatch();
        return false;
    }
    if (TaskSystem::IsRunning() && !m_batch->GetIsComplete())
    {
        return false;
    }
    bool arrived = false;
    for (size_t index = 0; index < m_batch->paths.size(); ++index)
    {
        const std::string& path = m_batch->paths[index];
        m_inFlight.erase(path);
        if (m_batch->results[index])
        {
            m_textures[path] = std::move(m_batch->results[index]);
            arrived = true;
        }
        else
        {
            m_failed.insert(path);
        }
    }
    m_batch.reset();
    StartBatch();
    return arrived;
}

void MaterialPreviewTextureCache::WaitForAll()
{
    while (m_batch)
    {
        if (TaskSystem::IsRunning())
        {
            TaskSystem::Scheduler().WaitforTask(m_batch.get());
        }
        Poll();
    }
}

size_t MaterialPreviewTextureCache::PendingCount() const
{
    return m_inFlight.size();
}

void MaterialPreviewTextureCache::Clear()
{
    if (m_batch && TaskSystem::IsRunning())
    {
        TaskSystem::Scheduler().WaitforTask(m_batch.get());
    }
    m_batch.reset();
    m_queued.clear();
    m_inFlight.clear();
    m_failed.clear();
    m_textures.clear();
}
}
