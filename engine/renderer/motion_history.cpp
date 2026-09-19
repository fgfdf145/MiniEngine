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
    frame.previousModels.reserve(models.size());

    std::unordered_map<uint64_t, glm::mat4> current;
    current.reserve(keys.size());
    for (size_t index = 0; index < keys.size(); ++index)
    {
        const uint64_t packed = Pack(keys[index]);
        if (!current.emplace(packed, models[index]).second)
        {
            throw std::invalid_argument("MotionHistory::Advance was given the same key twice");
        }

        const auto previous = m_hasHistory ? m_models.find(packed) : m_models.end();
        frame.previousModels.push_back(previous != m_models.end() ? previous->second : models[index]);
    }

    m_models = std::move(current);
    m_viewProjection = viewProjection;
    m_hasHistory = true;
    return frame;
}

void MotionHistory::Reset()
{
    m_hasHistory = false;
    m_models.clear();
}

uint64_t MotionHistory::Pack(const MotionKey& key)
{
    return (static_cast<uint64_t>(key.entity) << 32) | key.submeshOrdinal;
}
}
