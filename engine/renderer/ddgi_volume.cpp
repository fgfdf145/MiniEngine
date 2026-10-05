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
            m_levels[level].cold.assign(kDdgiProbesPerLevel, 0u);
            m_levels[level].hot.assign(kDdgiProbesPerLevel, 0u);
        }
        m_hot.clear();
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
        LevelState& state = m_levels[level];
        scheduled.push_back(PackDdgiProbe(level, index));
        ++updated[level];
        taken[level][index] = 1u;
        if (state.valid[index] == 0u || state.held[index] != coord)
        {
            // Another place: what the GPU said about the old one does not hold here.
            state.coldCount -= state.cold[index];
            state.cold[index] = 0u;
            state.hot[index] = 0u;
        }
        state.held[index] = coord;
        state.valid[index] = 1u;
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

    // Hot probes next, oldest first, within half the budget; the ones whose updates ran out leave.
    const size_t hotLimit = std::min<size_t>(budget, scheduled.size() + budget / 2);
    size_t keep = 0;
    for (const uint32_t packed : m_hot)
    {
        const uint32_t level = packed >> 24;
        const uint32_t index = packed & 0xffffffu;
        if (level >= levelCount || m_levels[level].hot[index] == 0u)
        {
            continue;
        }
        LevelState& state = m_levels[level];
        if (taken[level][index] == 0u && scheduled.size() < hotLimit)
        {
            take(level, index, DdgiSlotCoordinate(DdgiSlotFromIndex(index), levels[level].origin));
            // take clears it when the slot moved on to another coordinate.
            if (state.hot[index] > 0u)
            {
                --state.hot[index];
            }
        }
        if (state.hot[index] > 0u)
        {
            m_hot[keep++] = packed;
        }
    }
    m_hot.resize(keep);

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
            if (state.cursor == 0)
            {
                ++state.passes;
            }
            if (taken[level][index] != 0u || (state.cold[index] != 0u && (state.passes + index) % kDdgiColdRateDivisor != 0u))
            {
                continue;
            }
            take(level, index, DdgiSlotCoordinate(DdgiSlotFromIndex(index), levels[level].origin));
            --share;
        }
    }

    for (uint32_t level = 0; level < levelCount; ++level)
    {
        // Per probe that takes every round robin turn: the cold ones count as a fraction.
        const LevelState& state = m_levels[level];
        const float population = static_cast<float>(kDdgiProbesPerLevel - state.coldCount) +
                                 static_cast<float>(state.coldCount) / static_cast<float>(kDdgiColdRateDivisor);
        const float updatesPerProbe = static_cast<float>(updated[level]) / population;
        m_levels[level].residual *= std::pow(std::clamp(hysteresis, 0.0f, 1.0f), updatesPerProbe);
    }
    return scheduled;
}

void DdgiProbeScheduler::MakeHot(uint32_t level, uint32_t index, uint8_t updates)
{
    LevelState& state = m_levels[level];
    if (state.hot[index] == 0u)
    {
        m_hot.push_back(PackDdgiProbe(level, index));
    }
    state.hot[index] = std::max(state.hot[index], updates);
}

void DdgiProbeScheduler::ApplyFeedback(std::span<const uint32_t> scheduled, std::span<const uint32_t> feedback)
{
    const size_t count = std::min(scheduled.size(), feedback.size());
    for (size_t entry = 0; entry < count; ++entry)
    {
        const uint32_t level = scheduled[entry] >> 24;
        const uint32_t index = scheduled[entry] & 0xffffffu;
        if (level >= m_levels.size() || index >= kDdgiProbesPerLevel)
        {
            continue;
        }
        LevelState& state = m_levels[level];
        const uint32_t report = feedback[entry];
        if (state.valid[index] == 0u || DdgiFeedbackCoordinate(state.held[index]) != (report & ~0xffu))
        {
            continue;
        }
        // Only empty probes: a probe inside geometry needs its updates to come back to life when what
        // buried it moves (its back-face evidence fades over some 30 of them), and throttled, it left
        // surfaces next to it dark for minutes.
        const uint8_t cold = (report & kDdgiFeedbackEmpty) != 0u ? 1u : 0u;
        state.coldCount += cold;
        state.coldCount -= state.cold[index];
        state.cold[index] = cold;
        if ((report & kDdgiFeedbackChanged) != 0u)
        {
            // The level keeps its refresh rate: the probes that see the change come back on their
            // own. (A false alarm, a few in a million updates, would otherwise keep levels unsettled.)
            MakeHot(level, index, kDdgiHotUpdates);
            // The change most likely reaches past this probe: a door, a lamp or a building lights a
            // region. The neighbours look again soon and, when they see it too, pass it on.
            const glm::ivec3 slot = DdgiSlotFromIndex(index);
            constexpr std::array<glm::ivec3, 6> kSteps = {
                glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0), glm::ivec3(0, 1, 0), glm::ivec3(0, -1, 0), glm::ivec3(0, 0, 1), glm::ivec3(0, 0, -1)};
            for (const glm::ivec3& step : kSteps)
            {
                const glm::ivec3 neighbour = slot + step;
                // Slots wrap around the toroidal storage; the one across the edge is no neighbour.
                if (glm::any(glm::lessThan(neighbour, glm::ivec3(0))) || glm::any(glm::greaterThanEqual(neighbour, kDdgiGridSize)))
                {
                    continue;
                }
                const uint32_t neighbourIndex = DdgiSlotIndex(neighbour);
                if (state.valid[neighbourIndex] != 0u && state.held[neighbourIndex] - state.held[index] == step)
                {
                    MakeHot(level, neighbourIndex, kDdgiNeighbourHotUpdates);
                }
            }
        }
    }
}

void DdgiProbeScheduler::GeometryChanged()
{
    for (LevelState& state : m_levels)
    {
        std::fill(state.cold.begin(), state.cold.end(), uint8_t{0});
        state.coldCount = 0;
    }
    Unsettle();
}

bool DdgiProbeScheduler::Cold(uint32_t level, uint32_t slotIndex) const
{
    return level < m_levels.size() && m_levels[level].cold[slotIndex] != 0u;
}

bool DdgiProbeScheduler::Hot(uint32_t level, uint32_t slotIndex) const
{
    return level < m_levels.size() && m_levels[level].hot[slotIndex] != 0u;
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
    m_hot.clear();
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

bool DdgiLightingWatch::Update(std::span<const glm::vec4> lighting)
{
    bool changed = m_hasReference && lighting.size() != m_reference.size();
    for (size_t index = 0; m_hasReference && !changed && index < lighting.size(); ++index)
    {
        const glm::vec4 difference = glm::abs(lighting[index] - m_reference[index]);
        const glm::vec4 scale = glm::max(glm::max(glm::abs(lighting[index]), glm::abs(m_reference[index])), glm::vec4(1.0f));
        changed = glm::any(glm::greaterThan(difference, scale * kDdgiLightingTolerance));
    }
    if (changed || !m_hasReference)
    {
        m_reference.assign(lighting.begin(), lighting.end());
        m_hasReference = true;
    }
    m_changed = changed;
    if (changed)
    {
        m_epoch = (m_epoch + 1u) & 0xffu;
    }
    return changed;
}

bool DdgiLightingWatch::Changed() const
{
    return m_changed;
}

uint32_t DdgiLightingWatch::Epoch() const
{
    return m_epoch;
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
