#include "motion_history.h"

#include <stdexcept>

namespace me
{

MotionFrame MotionHistory::Advance(
    const glm::mat4& viewProjection,
    std::span<const MotionKey> keys,
    std::span<const glm::mat4> models)
{
    if (keys.size() != models.size())
    {
        throw std::invalid_argument("MotionHistory::Advance needs one model per key");
    }

    MotionFrame frame{};
    frame.previousViewProjection = m_hasHistory ? m_viewProjection : viewProjection;

    std::vector<uint64_t> packedKeys(keys.size());
    for (size_t index = 0; index < keys.size(); ++index)
    {
        packedKeys[index] = Pack(keys[index]);
    }

    if (m_hasHistory && packedKeys == m_keys)
    {
        // The same draws as last frame, which were checked for repeats when they came.
        frame.previousModels = std::move(m_orderedModels);
    }
    else
    {
        std::unordered_map<uint64_t, size_t> previousSlots;
        previousSlots.reserve(m_keys.size());
        for (size_t index = 0; index < m_keys.size(); ++index)
        {
            previousSlots.emplace(m_keys[index], index);
        }
        std::unordered_map<uint64_t, size_t> seen;
        seen.reserve(packedKeys.size());
        frame.previousModels.reserve(models.size());
        for (size_t index = 0; index < packedKeys.size(); ++index)
        {
            if (!seen.emplace(packedKeys[index], index).second)
            {
                throw std::invalid_argument("MotionHistory::Advance was given the same key twice");
            }
            const auto previous = m_hasHistory ? previousSlots.find(packedKeys[index]) : previousSlots.end();
            frame.previousModels.push_back(previous != previousSlots.end() ? m_orderedModels[previous->second] : models[index]);
        }
    }

    m_keys = std::move(packedKeys);
    m_orderedModels.assign(models.begin(), models.end());
    m_viewProjection = viewProjection;
    m_hasHistory = true;
    return frame;
}

void MotionHistory::Reset()
{
    m_hasHistory = false;
    m_keys.clear();
    m_orderedModels.clear();
}

uint64_t MotionHistory::Pack(const MotionKey& key)
{
    return (static_cast<uint64_t>(key.entity) << 32) | key.submeshOrdinal;
}
}
