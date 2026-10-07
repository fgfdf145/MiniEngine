#include "video_readback.h"

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

VkDeviceSize TexelBytes(VideoPixelFormat format)
{
    return format == VideoPixelFormat::RgbaHalf ? 8 : 4;
}

// The CPU reads every byte of every frame: cached memory reads fast, where write-combined memory,
// which is what host-visible memory without HOST_CACHED often is, reads many times slower.
uint32_t FindReadbackMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, bool& coherent)
{
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
    const VkMemoryPropertyFlags preferences[] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    };
    for (const VkMemoryPropertyFlags wanted : preferences)
    {
        for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
        {
            const VkMemoryPropertyFlags flags = properties.memoryTypes[index].propertyFlags;
            if ((typeFilter & (1u << index)) != 0 && (flags & wanted) == wanted)
            {
                coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
                return index;
            }
        }
    }
    throw std::runtime_error("No host-visible memory for the video recording");
}

void ImageBarrier(
    VkCommandBuffer commandBuffer,
    VkImage image,
    VkImageLayout from,
    VkImageLayout to,
    VkPipelineStageFlags sourceStage,
    VkAccessFlags sourceAccess,
    VkPipelineStageFlags destinationStage,
    VkAccessFlags destinationAccess)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}
}

VulkanVideoReadback::VulkanVideoReadback(VkPhysicalDevice physicalDevice, VkDevice device, uint32_t frameSlots)
    : m_physicalDevice(physicalDevice), m_device(device), m_slots(frameSlots)
{
}

VulkanVideoReadback::~VulkanVideoReadback()
{
    for (Slot& slot : m_slots)
    {
        Release(slot);
    }
}

bool VulkanVideoReadback::SupportsFormat(VkFormat format)
{
    return ToVideoPixelFormat(format).has_value();
}

void VulkanVideoReadback::RecordCopy(
    VkCommandBuffer commandBuffer,
    uint32_t frameSlot,
    VkImage image,
    VkFormat format,
    VkExtent2D extent,
    VkImageLayout layout,
    double timeSeconds)
{
    const std::optional<VideoPixelFormat> pixelFormat = ToVideoPixelFormat(format);
    if (!pixelFormat || frameSlot >= m_slots.size())
    {
        return;
    }
    Slot& slot = m_slots[frameSlot];
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(extent.width) * extent.height * TexelBytes(*pixelFormat);
    Reserve(slot, byteCount);

    // Whatever wrote or sampled the image before (the tone mapping pass, the editor's ImGui pass).
    ImageBarrier(
        commandBuffer, image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.buffer, 1, &region);
    ImageBarrier(
        commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);

    // The copy's writes, made visible to the host's reads once the fence is waited on.
    VkBufferMemoryBarrier bufferBarrier{};
    bufferBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bufferBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bufferBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferBarrier.buffer = slot.buffer;
    bufferBarrier.size = byteCount;
    vkCmdPipelineBarrier(
        commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bufferBarrier, 0, nullptr);

    slot.pending = true;
    slot.size = byteCount;
    slot.extent = extent;
    slot.format = *pixelFormat;
    slot.timeSeconds = timeSeconds;
}

void VulkanVideoReadback::RecordMosaicCopy(
    VkCommandBuffer commandBuffer,
    uint32_t frameSlot,
    std::span<const MosaicTile> tiles,
    VkFormat format,
    VkExtent2D canvasExtent,
    VkImageLayout layout,
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
    const VkDeviceSize texelBytes = TexelBytes(*pixelFormat);
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(canvasExtent.width) * canvasExtent.height * texelBytes;
    Reserve(slot, byteCount);

    // The canvas black where no tile covers it: opaque for the 8-bit formats, whose alpha is the top
    // byte of each little-endian word; zero (black) for half floats.
    vkCmdFillBuffer(commandBuffer, slot.buffer, 0, byteCount, texelBytes == 4 ? 0xFF000000u : 0u);
    VkBufferMemoryBarrier fillBarrier{};
    fillBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    fillBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fillBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fillBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fillBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fillBarrier.buffer = slot.buffer;
    fillBarrier.size = byteCount;
    vkCmdPipelineBarrier(
        commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &fillBarrier, 0, nullptr);

    // Each image straight into its place: the buffer's rows are the canvas's, so a tile's rows land
    // a canvas row apart.
    for (const MosaicTile& tile : tiles)
    {
        ImageBarrier(
            commandBuffer, tile.image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.bufferOffset = (static_cast<VkDeviceSize>(tile.y) * canvasExtent.width + tile.x) * texelBytes;
        region.bufferRowLength = canvasExtent.width;
        region.bufferImageHeight = canvasExtent.height;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {tile.extent.width, tile.extent.height, 1};
        vkCmdCopyImageToBuffer(commandBuffer, tile.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.buffer, 1, &region);
        ImageBarrier(
            commandBuffer, tile.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    }

    // The copies' writes, made visible to the host's reads once the fence is waited on.
    VkBufferMemoryBarrier bufferBarrier{};
    bufferBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bufferBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bufferBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferBarrier.buffer = slot.buffer;
    bufferBarrier.size = byteCount;
    vkCmdPipelineBarrier(
        commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bufferBarrier, 0, nullptr);

    slot.pending = true;
    slot.size = byteCount;
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
    if (!slot.coherent)
    {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = slot.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        CheckVulkan(vkInvalidateMappedMemoryRanges(m_device, 1, &range), "Failed to read the video frame back");
    }
    Frame frame;
    frame.extent = slot.extent;
    frame.frame.format = slot.format;
    frame.frame.timeSeconds = slot.timeSeconds;
    frame.frame.pixels.resize(static_cast<size_t>(slot.size));
    std::memcpy(frame.frame.pixels.data(), slot.mapped, frame.frame.pixels.size());
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

void VulkanVideoReadback::Reserve(Slot& slot, VkDeviceSize byteCount)
{
    if (slot.capacity >= byteCount)
    {
        return;
    }
    // The slot's last copy has completed: its fence was waited on before this frame was recorded.
    Release(slot);

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = byteCount;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CheckVulkan(vkCreateBuffer(m_device, &bufferInfo, nullptr, &slot.buffer), "Failed to create the video readback buffer");
    try
    {
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(m_device, slot.buffer, &requirements);
        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = FindReadbackMemoryType(m_physicalDevice, requirements.memoryTypeBits, slot.coherent);
        CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &slot.memory), "Failed to allocate the video readback buffer");
        CheckVulkan(vkBindBufferMemory(m_device, slot.buffer, slot.memory, 0), "Failed to bind the video readback buffer");
        CheckVulkan(vkMapMemory(m_device, slot.memory, 0, VK_WHOLE_SIZE, 0, &slot.mapped), "Failed to map the video readback buffer");
    }
    catch (...)
    {
        Release(slot);
        throw;
    }
    slot.capacity = byteCount;
}

void VulkanVideoReadback::Release(Slot& slot)
{
    if (slot.mapped != nullptr)
    {
        vkUnmapMemory(m_device, slot.memory);
        slot.mapped = nullptr;
    }
    if (slot.buffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(m_device, slot.buffer, nullptr);
        slot.buffer = VK_NULL_HANDLE;
    }
    if (slot.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(m_device, slot.memory, nullptr);
        slot.memory = VK_NULL_HANDLE;
    }
    slot.capacity = 0;
    slot.pending = false;
}
}
