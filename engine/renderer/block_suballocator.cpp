#include "block_suballocator.h"

#include <stdexcept>

namespace me
{

BlockSuballocator::BlockSuballocator(uint64_t size)
    : m_size(size)
{
    if (size > 0)
    {
        m_free.emplace(0, size);
    }
}

std::optional<uint64_t> BlockSuballocator::Allocate(uint64_t size, uint64_t alignment)
{
    if (size == 0)
    {
        return std::nullopt;
    }
    const uint64_t mask = alignment > 1 ? alignment - 1 : 0;
    for (auto it = m_free.begin(); it != m_free.end(); ++it)
    {
        const uint64_t rangeStart = it->first;
        const uint64_t rangeEnd = it->first + it->second;
        const uint64_t start = (rangeStart + mask) & ~mask;
        if (start < rangeStart || start > rangeEnd || rangeEnd - start < size)
        {
            continue;
        }
        const uint64_t end = start + size;
        m_free.erase(it);
        // The padding in front of an aligned start and the tail behind the range stay free.
        if (start > rangeStart)
        {
            m_free.emplace(rangeStart, start - rangeStart);
        }
        if (end < rangeEnd)
        {
            m_free.emplace(end, rangeEnd - end);
        }
        m_used += size;
        return start;
    }
    return std::nullopt;
}

void BlockSuballocator::Free(uint64_t offset, uint64_t size)
{
    if (size == 0 || offset + size > m_size || size > m_used)
    {
        throw std::logic_error("BlockSuballocator::Free: range was not allocated from this block");
    }
    uint64_t start = offset;
    uint64_t end = offset + size;

    auto next = m_free.lower_bound(start);
    if (next != m_free.end() && next->first < end)
    {
        throw std::logic_error("BlockSuballocator::Free: range overlaps a free range");
    }
    if (next != m_free.begin())
    {
        const auto previous = std::prev(next);
        const uint64_t previousEnd = previous->first + previous->second;
        if (previousEnd > start)
        {
            throw std::logic_error("BlockSuballocator::Free: range overlaps a free range");
        }
        if (previousEnd == start)
        {
            start = previous->first;
            m_free.erase(previous);
        }
    }
    if (next != m_free.end() && next->first == end)
    {
        end = next->first + next->second;
        m_free.erase(next);
    }
    m_free.emplace(start, end - start);
    m_used -= size;
}

uint64_t BlockSuballocator::Size() const
{
    return m_size;
}

uint64_t BlockSuballocator::UsedBytes() const
{
    return m_used;
}

bool BlockSuballocator::Empty() const
{
    return m_used == 0;
}

size_t BlockSuballocator::FreeRangeCount() const
{
    return m_free.size();
}
}
