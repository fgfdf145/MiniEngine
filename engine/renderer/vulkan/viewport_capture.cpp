#include "viewport_capture.h"

#include "upload_batch.h"

#include <stb_image_write.h>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace me
{

namespace
{
uint32_t FindHostVisibleMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter)
{
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
    {
        if ((typeFilter & (1u << index)) != 0 && (properties.memoryTypes[index].propertyFlags & wanted) == wanted)
        {
            return index;
        }
    }
    throw std::runtime_error("No host-visible memory for the viewport capture");
}

bool IsBgra(VkFormat format)
{
    return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
}

// The HDR output's LDR target: display-linear, 1.0 being SDR white (see hdr_composite.frag).
bool IsHalfFloat(VkFormat format)
{
    return format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_R16G16_SFLOAT;
}

// Channels of a half-float format IsHalfFloat accepts.
uint32_t HalfFloatChannels(VkFormat format)
{
    return format == VK_FORMAT_R16G16_SFLOAT ? 2u : 4u;
}

uint8_t EncodeSrgb(float linear)
{
    const float clamped = std::clamp(linear, 0.0f, 1.0f);
    const float encoded = clamped <= 0.0031308f ? clamped * 12.92f : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(encoded * 255.0f));
}

bool IsRgba(VkFormat format)
{
    return format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_R8G8B8A8_UNORM;
}

void TransitionForCopy(VkCommandBuffer commandBuffer, VkImage image, uint32_t layer, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier);
}

std::vector<uint8_t> ReadImageBytes(const ImageCaptureRequest& request)
{
    if (!IsBgra(request.format) && !IsRgba(request.format) && !IsHalfFloat(request.format))
    {
        throw std::runtime_error("Viewport capture supports only 8-bit RGBA and BGRA and half-float RGBA and RG images");
    }

    const VkDeviceSize texelBytes = IsHalfFloat(request.format) ? 2 * HalfFloatChannels(request.format) : 4;
    const VkDeviceSize byteCount = static_cast<VkDeviceSize>(request.extent.width) * request.extent.height * texelBytes;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = byteCount;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    CheckVulkan(vkCreateBuffer(request.device, &bufferInfo, nullptr, &buffer), "Failed to create the capture buffer");

    VkDeviceMemory memory = VK_NULL_HANDLE;
    try
    {
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(request.device, buffer, &requirements);
        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = FindHostVisibleMemoryType(request.physicalDevice, requirements.memoryTypeBits);
        CheckVulkan(vkAllocateMemory(request.device, &allocateInfo, nullptr, &memory), "Failed to allocate the capture buffer");
        CheckVulkan(vkBindBufferMemory(request.device, buffer, memory, 0), "Failed to bind the capture buffer");

        VulkanUploadBatch batch(request.device, request.queueFamily, request.queue);
        const VkCommandBuffer commandBuffer = batch.GetCommandBuffer();
        TransitionForCopy(commandBuffer, request.image, request.layer, request.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, request.layer, 1};
        region.imageExtent = {request.extent.width, request.extent.height, 1};
        vkCmdCopyImageToBuffer(commandBuffer, request.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
        TransitionForCopy(commandBuffer, request.image, request.layer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, request.layout);
        batch.Flush();

        std::vector<uint8_t> bytes(static_cast<size_t>(byteCount));
        void* mapped = nullptr;
        CheckVulkan(vkMapMemory(request.device, memory, 0, byteCount, 0, &mapped), "Failed to map the capture buffer");
        std::memcpy(bytes.data(), mapped, bytes.size());
        vkUnmapMemory(request.device, memory);
        vkDestroyBuffer(request.device, buffer, nullptr);
        vkFreeMemory(request.device, memory, nullptr);
        return bytes;
    }
    catch (...)
    {
        vkDestroyBuffer(request.device, buffer, nullptr);
        if (memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(request.device, memory, nullptr);
        }
        throw;
    }
}
}

std::vector<uint8_t> ReadBufferBytes(const ImageCaptureRequest& request, VkBuffer source, VkDeviceSize byteCount)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = byteCount;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    CheckVulkan(vkCreateBuffer(request.device, &bufferInfo, nullptr, &buffer), "Failed to create the readback buffer");
    VkDeviceMemory memory = VK_NULL_HANDLE;
    try
    {
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(request.device, buffer, &requirements);
        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = FindHostVisibleMemoryType(request.physicalDevice, requirements.memoryTypeBits);
        CheckVulkan(vkAllocateMemory(request.device, &allocateInfo, nullptr, &memory), "Failed to allocate the readback buffer");
        CheckVulkan(vkBindBufferMemory(request.device, buffer, memory, 0), "Failed to bind the readback buffer");

        VulkanUploadBatch batch(request.device, request.queueFamily, request.queue);
        const VkBufferCopy region{0, 0, byteCount};
        vkCmdCopyBuffer(batch.GetCommandBuffer(), source, buffer, 1, &region);
        batch.Flush();

        std::vector<uint8_t> bytes(static_cast<size_t>(byteCount));
        void* mapped = nullptr;
        CheckVulkan(vkMapMemory(request.device, memory, 0, byteCount, 0, &mapped), "Failed to map the readback buffer");
        std::memcpy(bytes.data(), mapped, bytes.size());
        vkUnmapMemory(request.device, memory);
        vkDestroyBuffer(request.device, buffer, nullptr);
        vkFreeMemory(request.device, memory, nullptr);
        return bytes;
    }
    catch (...)
    {
        vkDestroyBuffer(request.device, buffer, nullptr);
        if (memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(request.device, memory, nullptr);
        }
        throw;
    }
}

std::vector<glm::vec4> ReadImageHalfFloats(const ImageCaptureRequest& request)
{
    if (!IsHalfFloat(request.format))
    {
        throw std::runtime_error("Reading back floats needs a half-float RGBA image");
    }
    const std::vector<uint8_t> bytes = ReadImageBytes(request);
    // A two-channel image reads back with zero in b and a.
    const uint32_t channels = HalfFloatChannels(request.format);
    std::vector<glm::vec4> texels(bytes.size() / (2 * channels));
    for (size_t texel = 0; texel < texels.size(); ++texel)
    {
        uint16_t halves[4] = {};
        std::memcpy(halves, bytes.data() + texel * 2 * channels, 2 * channels);
        texels[texel] = glm::vec4(
            glm::unpackHalf1x16(halves[0]),
            glm::unpackHalf1x16(halves[1]),
            glm::unpackHalf1x16(halves[2]),
            glm::unpackHalf1x16(halves[3]));
    }
    return texels;
}

void CaptureImageToPng(const ImageCaptureRequest& request, const std::filesystem::path& path)
{
    const std::vector<uint8_t> bytes = ReadImageBytes(request);
    std::vector<uint8_t> pixels(static_cast<size_t>(request.extent.width) * request.extent.height * 4);
    if (IsHalfFloat(request.format))
    {
        // An SDR PNG of an HDR frame: clipped at UI white, sRGB-encoded.
        for (size_t texel = 0; texel < pixels.size(); ++texel)
        {
            uint16_t half = 0;
            std::memcpy(&half, bytes.data() + texel * 2, sizeof(half));
            pixels[texel] = EncodeSrgb(glm::unpackHalf1x16(half));
        }
    }
    else
    {
        std::memcpy(pixels.data(), bytes.data(), pixels.size());
    }
    if (IsBgra(request.format))
    {
        for (size_t texel = 0; texel < pixels.size(); texel += 4)
        {
            std::swap(pixels[texel], pixels[texel + 2]);
        }
    }
    for (size_t texel = 3; texel < pixels.size(); texel += 4)
    {
        pixels[texel] = 255;
    }

    if (stbi_write_png(
            path.string().c_str(),
            static_cast<int>(request.extent.width),
            static_cast<int>(request.extent.height),
            4,
            pixels.data(),
            static_cast<int>(request.extent.width) * 4) == 0)
    {
        throw std::runtime_error("Failed to write '" + path.string() + "'");
    }
}
}
