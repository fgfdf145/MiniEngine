#include "motion_history.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace me
{

namespace
{
// Last frame's keys and, as this frame's are looked up, which of them this frame has taken: open
// addressing in two flat arrays. A change of content on a streamed map relates tens of thousands of
// keys, and two node-based maps of them cost 3 ms of that frame.
class KeyTable
{
  public:
    static constexpr uint32_t kNotPrevious = UINT32_MAX - 1;

    explicit KeyTable(size_t keyCount)
    {
        const size_t size = std::bit_ceil(std::max<size_t>(16, keyCount * 2));
        m_keys.resize(size);
        m_values.assign(size, kEmpty);
        m_taken.assign(size, 0);
        m_mask = size - 1;
    }

    void InsertPrevious(uint64_t key, uint32_t index)
    {
        const size_t slot = Find(key);
        m_keys[slot] = key;
        m_values[slot] = index;
    }

    // The previous index of a key this frame names, kNotPrevious for a new one; false when this frame
    // named it already.
    bool Take(uint64_t key, uint32_t& previous)
    {
        const size_t slot = Find(key);
        if (m_values[slot] == kEmpty)
        {
            m_keys[slot] = key;
            m_values[slot] = kNotPrevious;
        }
        if (m_taken[slot] != 0)
        {
            return false;
        }
        m_taken[slot] = 1;
        previous = m_values[slot];
        return true;
    }

  private:
    static constexpr uint32_t kEmpty = UINT32_MAX;

    size_t Find(uint64_t key) const
    {
        // splitmix64's finalizer: entity ids and ordinals are small, sequential numbers.
        uint64_t hash = key;
        hash = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ull;
        hash = (hash ^ (hash >> 27)) * 0x94d049bb133111ebull;
        hash ^= hash >> 31;
        size_t slot = static_cast<size_t>(hash) & m_mask;
        while (m_values[slot] != kEmpty && m_keys[slot] != key)
        {
            slot = (slot + 1) & m_mask;
        }
        return slot;
    }

    std::vector<uint64_t> m_keys;
    std::vector<uint32_t> m_values;
    std::vector<uint8_t> m_taken;
    size_t m_mask = 0;
};
}

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
        const size_t previousCount = m_hasHistory ? m_keys.size() : 0;
        KeyTable table(previousCount + packedKeys.size());
        for (size_t index = 0; index < previousCount; ++index)
        {
            table.InsertPrevious(m_keys[index], static_cast<uint32_t>(index));
        }
        frame.previousModels.reserve(models.size());
        for (size_t index = 0; index < packedKeys.size(); ++index)
        {
            uint32_t previous = KeyTable::kNotPrevious;
            if (!table.Take(packedKeys[index], previous))
            {
                throw std::invalid_argument("MotionHistory::Advance was given the same key twice");
            }
            frame.previousModels.push_back(previous != KeyTable::kNotPrevious ? m_orderedModels[previous] : models[index]);
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
