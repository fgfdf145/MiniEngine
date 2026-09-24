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

// The HDR output's LDR target: display-linear, 1.0 being UI white (see hdr_output.glsl).
bool IsHalfFloat(VkFormat format)
{
    return format == VK_FORMAT_R16G16B16A16_SFLOAT;
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

void TransitionForCopy(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
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
}

void CaptureImageToPng(const ImageCaptureRequest& request, const std::filesystem::path& path)
{
    if (!IsBgra(request.format) && !IsRgba(request.format) && !IsHalfFloat(request.format))
    {
        throw std::runtime_error("Viewport capture supports only 8-bit RGBA and BGRA and half-float RGBA images");
    }

    const VkDeviceSize texelBytes = IsHalfFloat(request.format) ? 8 : 4;
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
        TransitionForCopy(commandBuffer, request.image, request.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {request.extent.width, request.extent.height, 1};
        vkCmdCopyImageToBuffer(commandBuffer, request.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
        TransitionForCopy(commandBuffer, request.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, request.layout);
        batch.Flush();

        std::vector<uint8_t> pixels(static_cast<size_t>(request.extent.width) * request.extent.height * 4);
        void* mapped = nullptr;
        CheckVulkan(vkMapMemory(request.device, memory, 0, byteCount, 0, &mapped), "Failed to map the capture buffer");
        if (IsHalfFloat(request.format))
        {
            // An SDR PNG of an HDR frame: clipped at UI white, sRGB-encoded.
            const auto* halves = static_cast<const uint16_t*>(mapped);
            for (size_t texel = 0; texel < pixels.size(); ++texel)
            {
                pixels[texel] = EncodeSrgb(glm::unpackHalf1x16(halves[texel]));
            }
        }
        else
        {
            std::memcpy(pixels.data(), mapped, pixels.size());
        }
        vkUnmapMemory(request.device, memory);
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
    catch (...)
    {
        vkDestroyBuffer(request.device, buffer, nullptr);
        if (memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(request.device, memory, nullptr);
        }
        throw;
    }
    vkDestroyBuffer(request.device, buffer, nullptr);
    vkFreeMemory(request.device, memory, nullptr);
}
}
