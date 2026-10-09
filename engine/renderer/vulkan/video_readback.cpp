#include "video_readback.h"

#include "nvrhi_native.h"

#include <cstring>
#include <stdexcept>

namespace me
{

namespace
{
std::optional<VideoPixelFormat> ToVideoPixelFormat(VkFormat format)
{
    switch (format)
    {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM:
        return VideoPixelFormat::Rgba8;
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
        return VideoPixelFormat::Bgra8;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return VideoPixelFormat::RgbaHalf;
    default:
        return std::nullopt;
    }
}

size_t TexelBytes(VideoPixelFormat format)
{
    return format == VideoPixelFormat::RgbaHalf ? 8 : 4;
}

// Copies image into staging at (x, y), the image a copy source for the copy and a shader resource
// after (where its readers find it). The frame's list has no automatic barriers.
void RecordTileCopy(
    nvrhi::ICommandList* commandList, nvrhi::IStagingTexture* staging, nvrhi::ITexture* image, VkExtent2D extent, uint32_t x, uint32_t y)
{
    commandList->setTextureState(image, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
    commandList->commitBarriers();
    commandList->copyTexture(
        staging,
        nvrhi::TextureSlice().setOrigin(x, y).setWidth(extent.width).setHeight(extent.height),
        image,
        nvrhi::TextureSlice().setWidth(extent.width).setHeight(extent.height));
    commandList->setTextureState(image, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();
}
}

VulkanVideoReadback::VulkanVideoReadback(nvrhi::IDevice* device, uint32_t frameSlots)
    : m_device(device), m_slots(frameSlots)
{
}

VulkanVideoReadback::~VulkanVideoReadback() = default;

bool VulkanVideoReadback::SupportsFormat(VkFormat format)
{
    return ToVideoPixelFormat(format).has_value();
}

void VulkanVideoReadback::RecordCopy(
    nvrhi::ICommandList* commandList,
    uint32_t frameSlot,
    nvrhi::ITexture* image,
    VkFormat format,
    VkExtent2D extent,
    double timeSeconds)
{
    const std::optional<VideoPixelFormat> pixelFormat = ToVideoPixelFormat(format);
    if (!pixelFormat || frameSlot >= m_slots.size())
    {
        return;
    }
    Slot& slot = m_slots[frameSlot];
    Reserve(slot, extent, format);
    RecordTileCopy(commandList, slot.staging, image, extent, 0, 0);
    slot.pending = true;
    slot.extent = extent;
    slot.format = *pixelFormat;
    slot.timeSeconds = timeSeconds;
    slot.written = {Rect{0, 0, extent.width, extent.height}};
}

void VulkanVideoReadback::RecordMosaicCopy(
    nvrhi::ICommandList* commandList,
    uint32_t frameSlot,
    std::span<const MosaicTile> tiles,
    VkFormat format,
    VkExtent2D canvasExtent,
    double timeSeconds)
{
    const std::optional<VideoPixelFormat> pixelFormat = ToVideoPixelFormat(format);
    if (!pixelFormat || frameSlot >= m_slots.size())
    {
        return;
    }
    for (const MosaicTile& tile : tiles)
    {
        if (tile.x + tile.extent.width > canvasExtent.width || tile.y + tile.extent.height > canvasExtent.height)
        {
            throw std::runtime_error("A mosaic tile lies outside its canvas");
        }
    }
    Slot& slot = m_slots[frameSlot];
    Reserve(slot, canvasExtent, format);
    slot.written.clear();
    // Each image straight into its place on the canvas.
    for (const MosaicTile& tile : tiles)
    {
        RecordTileCopy(commandList, slot.staging, tile.image, tile.extent, tile.x, tile.y);
        slot.written.push_back(Rect{tile.x, tile.y, tile.extent.width, tile.extent.height});
    }
    slot.pending = true;
    slot.extent = canvasExtent;
    slot.format = *pixelFormat;
    slot.timeSeconds = timeSeconds;
}

std::optional<VulkanVideoReadback::Frame> VulkanVideoReadback::Take(uint32_t frameSlot)
{
    if (frameSlot >= m_slots.size() || !m_slots[frameSlot].pending)
    {
        return std::nullopt;
    }
    Slot& slot = m_slots[frameSlot];
    slot.pending = false;
    const size_t texelBytes = TexelBytes(slot.format);
    Frame frame;
    frame.extent = slot.extent;
    frame.frame.format = slot.format;
    frame.frame.timeSeconds = slot.timeSeconds;
    frame.frame.pixels.resize(static_cast<size_t>(slot.extent.width) * slot.extent.height * texelBytes);
    // Black where no image was copied: opaque for the 8-bit formats, whose alpha is the top byte of
    // each little-endian word; zero (black) for half floats.
    if (texelBytes == 4)
    {
        auto* words = reinterpret_cast<uint32_t*>(frame.frame.pixels.data());
        std::fill(words, words + frame.frame.pixels.size() / 4, 0xFF000000u);
    }
    else
    {
        std::fill(frame.frame.pixels.begin(), frame.frame.pixels.end(), uint8_t{0});
    }
    size_t rowPitch = 0;
    const auto* mapped = static_cast<const uint8_t*>(m_device->mapStagingTexture(slot.staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
    if (mapped == nullptr)
    {
        throw std::runtime_error("Failed to read the video frame back");
    }
    const size_t canvasRowBytes = slot.extent.width * texelBytes;
    for (const Rect& rect : slot.written)
    {
        const size_t rowBytes = rect.width * texelBytes;
        for (uint32_t row = 0; row < rect.height; ++row)
        {
            const size_t offset = (rect.y + row) * canvasRowBytes + rect.x * texelBytes;
            std::memcpy(frame.frame.pixels.data() + offset, mapped + (rect.y + row) * rowPitch + rect.x * texelBytes, rowBytes);
        }
    }
    m_device->unmapStagingTexture(slot.staging);
    return frame;
}

bool VulkanVideoReadback::HasPending() const
{
    for (const Slot& slot : m_slots)
    {
        if (slot.pending)
        {
            return true;
        }
    }
    return false;
}

void VulkanVideoReadback::Reserve(Slot& slot, VkExtent2D extent, VkFormat format)
{
    if (slot.staging && slot.capacity.width >= extent.width && slot.capacity.height >= extent.height && slot.stagingFormat == format)
    {
        return;
    }
    // The slot's last copy has completed: its frame was waited for before this one was recorded.
    nvrhi::TextureDesc desc;
    desc.width = extent.width;
    desc.height = extent.height;
    desc.format = ToNvrhiFormat(format);
    desc.dimension = nvrhi::TextureDimension::Texture2D;
    desc.debugName = "Video readback";
    slot.staging = m_device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
    if (!slot.staging)
    {
        throw std::runtime_error("Failed to create the video readback staging texture");
    }
    slot.capacity = extent;
    slot.stagingFormat = format;
    slot.pending = false;
}
}
