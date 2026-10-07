#pragma once

#include "common.h"

#include <engine/core/video/video_recorder.h>

#include <optional>
#include <span>
#include <vector>

namespace me
{

// Reads the frames a video recording wants back from the GPU without stalling it: each frame slot
// has a host-visible buffer the frame's command buffer copies the image into, read once the slot's
// fence says that frame finished, frames in flight later.
class VulkanVideoReadback
{
  public:
    VulkanVideoReadback(VkPhysicalDevice physicalDevice, VkDevice device, uint32_t frameSlots);
    ~VulkanVideoReadback();

    VulkanVideoReadback(const VulkanVideoReadback&) = delete;
    VulkanVideoReadback& operator=(const VulkanVideoReadback&) = delete;

    // Whether a recording can take images of this format.
    static bool SupportsFormat(VkFormat format);

    // Records the copy of image (created with VK_IMAGE_USAGE_TRANSFER_SRC_BIT, in layout and
    // returned to it) into the slot's buffer, after everything the command buffer did before.
    void RecordCopy(
        VkCommandBuffer commandBuffer,
        uint32_t frameSlot,
        VkImage image,
        VkFormat format,
        VkExtent2D extent,
        VkImageLayout layout,
        double timeSeconds);

    // One image of a mosaic: where its top-left corner goes on the canvas, in pixels.
    struct MosaicTile
    {
        VkImage image = VK_NULL_HANDLE;
        VkExtent2D extent{};
        uint32_t x = 0;
        uint32_t y = 0;
    };
    // Records the copy of several images of one format, each in layout and returned to it, into one
    // canvas of canvasExtent in the slot's buffer, the rest of it black: a quad recording's frame
    // (docs/design/2026-10-07-quad-vehicle-recording-design.md). The tiles must lie inside the canvas.
    void RecordMosaicCopy(
        VkCommandBuffer commandBuffer,
        uint32_t frameSlot,
        std::span<const MosaicTile> tiles,
        VkFormat format,
        VkExtent2D canvasExtent,
        VkImageLayout layout,
        double timeSeconds);

    struct Frame
    {
        VideoFrame frame;
        VkExtent2D extent{};
    };
    // The slot's copy, once the slot's fence has been waited on since RecordCopy; empty when the
    // slot has none.
    std::optional<Frame> Take(uint32_t frameSlot);
    bool HasPending() const;

  private:
    struct Slot
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize capacity = 0;
        bool coherent = false;
        bool pending = false;
        VkDeviceSize size = 0;
        VkExtent2D extent{};
        VideoPixelFormat format = VideoPixelFormat::Rgba8;
        double timeSeconds = 0.0;
    };

    // Makes the slot's buffer at least byteCount bytes.
    void Reserve(Slot& slot, VkDeviceSize byteCount);
    void Release(Slot& slot);

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    std::vector<Slot> m_slots;
};
}
