#include "ddgi_volume.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace me
{

namespace
{
int FloorMod(int value, int size)
{
    const int remainder = value % size;
    return remainder < 0 ? remainder + size : remainder;
}

// A 32-bit integer hash (Wellons' lowbias32), for the per-frame rotation.
uint32_t Hash(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

float UnitFloat(uint32_t value)
{
    return static_cast<float>(value >> 8) * (1.0f / 16777216.0f);
}
}

DdgiLevel ComputeDdgiLevel(const glm::vec3& camera, float spacing)
{
    DdgiLevel level{};
    level.spacing = spacing;
    const glm::ivec3 cell(glm::floor(camera / spacing));
    level.origin = cell - (kDdgiGridSize / 2 - 1);
    return level;
}

glm::ivec3 DdgiStorageSlot(const glm::ivec3& coord)
{
    return glm::ivec3(FloorMod(coord.x, kDdgiGridSize.x), FloorMod(coord.y, kDdgiGridSize.y), FloorMod(coord.z, kDdgiGridSize.z));
}

glm::ivec3 DdgiSlotCoordinate(const glm::ivec3& slot, const glm::ivec3& origin)
{
    return origin + DdgiStorageSlot(slot - origin);
}

uint32_t DdgiSlotIndex(const glm::ivec3& slot)
{
    return static_cast<uint32_t>(slot.x + kDdgiGridSize.x * (slot.z + kDdgiGridSize.z * slot.y));
}

glm::ivec3 DdgiSlotFromIndex(uint32_t index)
{
    const int value = static_cast<int>(index);
    const int x = value % kDdgiGridSize.x;
    const int z = (value / kDdgiGridSize.x) % kDdgiGridSize.z;
    const int y = value / (kDdgiGridSize.x * kDdgiGridSize.z);
    return glm::ivec3(x, y, z);
}

std::vector<uint32_t> DdgiProbeScheduler::Schedule(std::span<const DdgiLevel> levels, uint32_t budget, float hysteresis)
{
    const size_t levelCount = std::min<size_t>(levels.size(), kDdgiMaxLevels);
    bool restart = levelCount != m_levels.size();
    for (size_t level = 0; level < levelCount && !restart; ++level)
    {
        restart = m_levels[level].spacing != levels[level].spacing;
    }
    if (restart)
    {
        m_levels.assign(levelCount, LevelState{});
        for (size_t level = 0; level < levelCount; ++level)
        {
            m_levels[level].spacing = levels[level].spacing;
            m_levels[level].held.assign(kDdgiProbesPerLevel, glm::ivec3(0));
            m_levels[level].valid.assign(kDdgiProbesPerLevel, 0u);
        }
    }

    for (size_t level = 0; level < levelCount; ++level)
    {
        m_levels[level].origin = levels[level].origin;
    }

    std::vector<uint32_t> scheduled;
    scheduled.reserve(budget);
    std::array<uint32_t, kDdgiMaxLevels> updated{};
    // Marks what this frame updates, so round robin does not pick it twice.
    std::vector<std::vector<uint8_t>> taken(levelCount, std::vector<uint8_t>(kDdgiProbesPerLevel, 0u));
    const auto take = [&](uint32_t level, uint32_t index, const glm::ivec3& coord)
    {
        scheduled.push_back(PackDdgiProbe(level, index));
        ++updated[level];
        taken[level][index] = 1u;
        m_levels[level].held[index] = coord;
        m_levels[level].valid[index] = 1u;
    };

    // Stale probes first, finest level first.
    for (uint32_t level = 0; level < levelCount && scheduled.size() < budget; ++level)
    {
        LevelState& state = m_levels[level];
        for (uint32_t index = 0; index < kDdgiProbesPerLevel && scheduled.size() < budget; ++index)
        {
            const glm::ivec3 coord = DdgiSlotCoordinate(DdgiSlotFromIndex(index), levels[level].origin);
            if (state.valid[index] == 0u || state.held[index] != coord)
            {
                take(level, index, coord);
                // New probes in the level: it starts converging over.
                state.residual = 1.0f;
            }
        }
    }

    // The rest round robin: level l weighs 2^(count - 1 - l).
    const uint32_t remaining = budget - static_cast<uint32_t>(scheduled.size());
    float weightSum = 0.0f;
    for (size_t level = 0; level < levelCount; ++level)
    {
        weightSum += std::ldexp(1.0f, static_cast<int>(levelCount - 1 - level));
    }
    for (uint32_t level = 0; level < levelCount && remaining > 0; ++level)
    {
        LevelState& state = m_levels[level];
        const float settledScale = state.residual < kDdgiSettledResidual ? 1.0f / kDdgiSettledShareDivisor : 1.0f;
        state.credit += static_cast<float>(remaining) * std::ldexp(1.0f, static_cast<int>(levelCount - 1 - level)) / weightSum * settledScale;
        uint32_t share = static_cast<uint32_t>(state.credit);
        state.credit -= static_cast<float>(share);
        share = std::min(share, kDdgiProbesPerLevel);
        for (uint32_t visited = 0; visited < kDdgiProbesPerLevel && share > 0 && scheduled.size() < budget; ++visited)
        {
            const uint32_t index = state.cursor;
            state.cursor = (state.cursor + 1) % kDdgiProbesPerLevel;
            if (taken[level][index] != 0u)
            {
                continue;
            }
            take(level, index, DdgiSlotCoordinate(DdgiSlotFromIndex(index), levels[level].origin));
            --share;
        }
    }

    for (uint32_t level = 0; level < levelCount; ++level)
    {
        const float updatesPerProbe = static_cast<float>(updated[level]) / static_cast<float>(kDdgiProbesPerLevel);
        m_levels[level].residual *= std::pow(std::clamp(hysteresis, 0.0f, 1.0f), updatesPerProbe);
    }
    return scheduled;
}

void DdgiProbeScheduler::Unsettle()
{
    for (LevelState& state : m_levels)
    {
        state.residual = 1.0f;
    }
}

bool DdgiProbeScheduler::Settled(uint32_t level) const
{
    return level < m_levels.size() && m_levels[level].residual < kDdgiSettledResidual;
}

void DdgiProbeScheduler::Reset()
{
    m_levels.clear();
}

uint32_t DdgiProbeScheduler::StaleCount(uint32_t level) const
{
    if (level >= m_levels.size())
    {
        return kDdgiProbesPerLevel;
    }
    const LevelState& state = m_levels[level];
    uint32_t stale = 0;
    for (uint32_t index = 0; index < kDdgiProbesPerLevel; ++index)
    {
        if (state.valid[index] == 0u || state.held[index] != DdgiSlotCoordinate(DdgiSlotFromIndex(index), state.origin))
        {
            ++stale;
        }
    }
    return stale;
}

std::span<const uint8_t> DdgiMovingInstances::Update(std::span<const glm::mat4> models)
{
    if (models.size() != m_previous.size())
    {
        m_previous.assign(models.begin(), models.end());
        m_stillFrames.assign(models.size(), kDdgiMovingInstanceFrames);
        m_skipped.assign(models.size(), 0u);
        return m_skipped;
    }
    for (size_t index = 0; index < models.size(); ++index)
    {
        if (models[index] != m_previous[index])
        {
            m_previous[index] = models[index];
            m_stillFrames[index] = 0;
        }
        else if (m_stillFrames[index] < kDdgiMovingInstanceFrames)
        {
            ++m_stillFrames[index];
        }
        m_skipped[index] = m_stillFrames[index] < kDdgiMovingInstanceFrames ? 1u : 0u;
    }
    return m_skipped;
}

float DdgiAdaptiveHysteresis::Update(std::span<const glm::vec4> lighting, float seconds, float hysteresis)
{
    bool changed = m_hasPrevious && lighting.size() != m_previous.size();
    for (size_t index = 0; m_hasPrevious && !changed && index < lighting.size(); ++index)
    {
        const glm::vec4 difference = glm::abs(lighting[index] - m_previous[index]);
        const glm::vec4 scale = glm::max(glm::max(glm::abs(lighting[index]), glm::abs(m_previous[index])), glm::vec4(1.0f));
        changed = glm::any(glm::greaterThan(difference, scale * 1e-3f));
    }
    m_previous.assign(lighting.begin(), lighting.end());
    m_hasPrevious = true;
    m_changed = changed;

    if (changed)
    {
        m_fastSecondsLeft = kDdgiFastSeconds;
    }
    else
    {
        m_fastSecondsLeft = std::max(m_fastSecondsLeft - std::max(seconds, 0.0f), 0.0f);
    }
    return m_fastSecondsLeft > 0.0f ? std::min(hysteresis, kDdgiFastHysteresis) : hysteresis;
}

bool DdgiAdaptiveHysteresis::Changed() const
{
    return m_changed;
}

glm::mat3 DdgiRayRotation(uint32_t frameIndex)
{
    // A uniformly random unit quaternion (Shoemake's method).
    const float u1 = UnitFloat(Hash(frameIndex * 3u + 1u));
    const float u2 = UnitFloat(Hash(frameIndex * 3u + 2u));
    const float u3 = UnitFloat(Hash(frameIndex * 3u + 3u));
    constexpr float kTwoPi = 6.28318530718f;
    const float a = std::sqrt(1.0f - u1);
    const float b = std::sqrt(u1);
    const glm::quat rotation(a * std::cos(kTwoPi * u2), a * std::sin(kTwoPi * u2), b * std::sin(kTwoPi * u3), b * std::cos(kTwoPi * u3));
    return glm::mat3_cast(glm::normalize(rotation));
}
}
