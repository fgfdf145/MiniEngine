#pragma once

#include "common.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <vector>

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
    // The array layer to read.
    uint32_t layer = 0;
};

// Copies a colour image to host memory and writes it as an RGBA PNG. The caller makes sure the
// GPU is idle and the image was created with VK_IMAGE_USAGE_TRANSFER_SRC_BIT. Throws
// std::runtime_error on failure.
void CaptureImageToPng(const ImageCaptureRequest& request, const std::filesystem::path& path);

// Copies a half-float RGBA image's first array layer to host memory, row by row from the top. Same
// requirements.
std::vector<glm::vec4> ReadImageHalfFloats(const ImageCaptureRequest& request);

// Copies byteCount bytes of a buffer created with VK_BUFFER_USAGE_TRANSFER_SRC_BIT to host memory,
// on the request's device and queue (its image fields are unused). The GPU must be idle.
std::vector<uint8_t> ReadBufferBytes(const ImageCaptureRequest& request, VkBuffer source, VkDeviceSize byteCount);
}
