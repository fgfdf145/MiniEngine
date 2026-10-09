#pragma once

#include "common.h"

#include <nvrhi/nvrhi.h>

#include <glm/glm.hpp>

#include <filesystem>
#include <vector>

namespace me
{

struct ImageCaptureRequest
{
    nvrhi::IDevice* device = nullptr;
    // NVRHI tracks its state (keepInitialState): the copy moves it to a copy source and back.
    nvrhi::ITexture* texture = nullptr;
    // R8G8B8A8_* or B8G8R8A8_*; the bytes are written as stored, so an _SRGB image yields
    // display-encoded PNG values.
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    // The array layer to read.
    uint32_t layer = 0;
};

// Copies a colour image to host memory and writes it as an RGBA PNG. The caller makes sure the
// GPU is idle. Throws std::runtime_error on failure.
void CaptureImageToPng(const ImageCaptureRequest& request, const std::filesystem::path& path);

// The same image as RGBA8 bytes, rows from the top, alpha opaque, as CaptureImageToPng writes them.
std::vector<uint8_t> ReadImageRgba8(const ImageCaptureRequest& request);

// Copies a half-float RGBA image's array layer to host memory, row by row from the top. Same
// requirements.
std::vector<glm::vec4> ReadImageHalfFloats(const ImageCaptureRequest& request);

// Copies byteCount bytes of a buffer to host memory on the request's device (its image fields are
// unused). The GPU must be idle.
std::vector<uint8_t> ReadBufferBytes(const ImageCaptureRequest& request, nvrhi::IBuffer* source, uint64_t byteCount);
}
