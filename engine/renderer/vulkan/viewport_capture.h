#pragma once

#include "common.h"

#include <filesystem>

namespace me
{

struct ImageCaptureRequest
{
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    // R8G8B8A8_* or B8G8R8A8_*; the bytes are written as stored, so an _SRGB image yields
    // display-encoded PNG values.
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    // The layout the image is in, and is returned to, around the copy.
    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};

// Copies a colour image to host memory and writes it as an RGBA PNG. The caller makes sure the
// GPU is idle and the image was created with VK_IMAGE_USAGE_TRANSFER_SRC_BIT. Throws
// std::runtime_error on failure.
void CaptureImageToPng(const ImageCaptureRequest& request, const std::filesystem::path& path);
}
