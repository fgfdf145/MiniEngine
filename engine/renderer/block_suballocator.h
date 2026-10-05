#pragma once

#include <cstdint>
#include <map>
#include <optional>

namespace me
{

// Hands out aligned ranges of one fixed-size block, first fit, and merges freed ranges with their
// free neighbours. Backend-agnostic: the Vulkan memory pool keeps one per VkDeviceMemory block, so
// thousands of small buffers and images share a few large allocations instead of each paying the
// driver's per-allocation granularity.
class BlockSuballocator
{
  public:
    explicit BlockSuballocator(uint64_t size);

    // The offset of a range of size bytes starting on a multiple of alignment (a power of two, or 0
    // and 1 for none), or nothing when no free range is large enough.
    std::optional<uint64_t> Allocate(uint64_t size, uint64_t alignment);
    // Returns a range Allocate handed out, with the same size.
    void Free(uint64_t offset, uint64_t size);

    uint64_t Size() const;
    uint64_t UsedBytes() const;
    bool Empty() const;
    // Separate free ranges; 1 for an empty block.
    size_t FreeRangeCount() const;

  private:
    uint64_t m_size = 0;
    uint64_t m_used = 0;
    std::map<uint64_t, uint64_t> m_free; // offset -> size, never adjacent to one another
};
}
