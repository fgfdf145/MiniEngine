#pragma once

#include "common.h"

#include <engine/core/video/video_recorder.h>

#include <nvrhi/nvrhi.h>

#include <optional>
#include <span>
#include <vector>

namespace me
{

// Reads the frames a video recording wants back from the GPU without stalling it: each frame slot
// has a staging texture the frame's command list copies the image (or the images of a mosaic) into,
// read once the slot's frame has finished, frames in flight later.
class VulkanVideoReadback
{
  public:
    VulkanVideoReadback(nvrhi::IDevice* device, uint32_t frameSlots);
    ~VulkanVideoReadback();

    VulkanVideoReadback(const VulkanVideoReadback&) = delete;
    VulkanVideoReadback& operator=(const VulkanVideoReadback&) = delete;

    // Whether a recording can take images of this format.
    static bool SupportsFormat(VkFormat format);

    // Records the copy of image into the slot's staging texture, after everything the command list
    // did before; the image goes back to being a shader resource after.
    void RecordCopy(
        nvrhi::ICommandList* commandList,
        uint32_t frameSlot,
        nvrhi::ITexture* image,
        VkFormat format,
        VkExtent2D extent,
        double timeSeconds);

    // One image of a mosaic: where its top-left corner goes on the canvas, in pixels.
    struct MosaicTile
    {
        nvrhi::ITexture* image = nullptr;
        VkExtent2D extent{};
        uint32_t x = 0;
        uint32_t y = 0;
    };
    // Records the copy of several images of one format into one canvas of canvasExtent in the slot's
    // staging texture, the rest of it black: a quad recording's frame
    // (docs/design/2026-10-07-quad-vehicle-recording-design.md). The tiles must lie inside the canvas.
    void RecordMosaicCopy(
        nvrhi::ICommandList* commandList,
        uint32_t frameSlot,
        std::span<const MosaicTile> tiles,
        VkFormat format,
        VkExtent2D canvasExtent,
        double timeSeconds);

    struct Frame
    {
        VideoFrame frame;
        VkExtent2D extent{};
    };
    // The slot's copy, once the slot's frame has completed since RecordCopy; empty when the slot has
    // none.
    std::optional<Frame> Take(uint32_t frameSlot);
    bool HasPending() const;

  private:
    struct Rect
    {
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    struct Slot
    {
        nvrhi::StagingTextureHandle staging;
        VkExtent2D capacity{};
        VkFormat stagingFormat = VK_FORMAT_UNDEFINED;
        bool pending = false;
        VkExtent2D extent{};
        VideoPixelFormat format = VideoPixelFormat::Rgba8;
        double timeSeconds = 0.0;
        // What the copies wrote; the rest of the canvas is black.
        std::vector<Rect> written;
    };

    // Makes the slot's staging texture at least extent, of format.
    void Reserve(Slot& slot, VkExtent2D extent, VkFormat format);

    nvrhi::IDevice* m_device = nullptr;
    std::vector<Slot> m_slots;
};
}
