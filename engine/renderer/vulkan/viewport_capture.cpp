#include "viewport_capture.h"


#include <stb_image_write.h>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <vector>

namespace me
{

namespace
{
bool IsBgra(VkFormat format)
{
    return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
}

// The HDR output's LDR target: display-linear, 1.0 being UI white (see hdr_output.slang).
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

// Runs commands on the request's device at once and waits for them.
void RunNow(nvrhi::IDevice* device, const std::function<void(nvrhi::ICommandList*)>& record)
{
    nvrhi::CommandListHandle commandList = device->createCommandList();
    if (!commandList)
    {
        throw std::runtime_error("Failed to create the capture command list");
    }
    commandList->open();
    record(commandList);
    commandList->close();
    device->executeCommandList(commandList);
    device->waitForIdle();
}

std::vector<uint8_t> ReadImageBytes(const ImageCaptureRequest& request)
{
    if (!IsBgra(request.format) && !IsRgba(request.format) && !IsHalfFloat(request.format))
    {
        throw std::runtime_error("Viewport capture supports only 8-bit RGBA and BGRA and half-float RGBA and RG images");
    }
    const size_t texelBytes = IsHalfFloat(request.format) ? 2 * HalfFloatChannels(request.format) : 4;
    nvrhi::IDevice* device = request.device;

    // A staging copy of the region, the texture moved to a copy source and back (NVRHI's automatic
    // barriers, from the state it tracks).
    nvrhi::TextureDesc desc = request.texture->getDesc();
    desc.width = request.extent.width;
    desc.height = request.extent.height;
    desc.depth = 1;
    desc.arraySize = 1;
    desc.mipLevels = 1;
    desc.dimension = nvrhi::TextureDimension::Texture2D;
    desc.isRenderTarget = false;
    desc.isUAV = false;
    desc.isTypeless = false;
    desc.keepInitialState = false;
    desc.initialState = nvrhi::ResourceStates::Unknown;
    desc.debugName = "Capture staging";
    nvrhi::StagingTextureHandle staging = device->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
    if (!staging)
    {
        throw std::runtime_error("Failed to create the capture staging texture");
    }
    RunNow(device, [&](nvrhi::ICommandList* commandList)
           {
               commandList->copyTexture(
                   staging,
                   nvrhi::TextureSlice().setWidth(request.extent.width).setHeight(request.extent.height),
                   request.texture,
                   nvrhi::TextureSlice().setArraySlice(request.layer).setWidth(request.extent.width).setHeight(request.extent.height));
           });

    size_t rowPitch = 0;
    const auto* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
    if (mapped == nullptr)
    {
        throw std::runtime_error("Failed to map the capture staging texture");
    }
    const size_t rowBytes = request.extent.width * texelBytes;
    std::vector<uint8_t> bytes(rowBytes * request.extent.height);
    for (uint32_t row = 0; row < request.extent.height; ++row)
    {
        std::memcpy(bytes.data() + row * rowBytes, mapped + row * rowPitch, rowBytes);
    }
    device->unmapStagingTexture(staging);
    return bytes;
}
}

std::vector<uint8_t> ReadBufferBytes(const ImageCaptureRequest& request, nvrhi::IBuffer* source, uint64_t byteCount)
{
    nvrhi::IDevice* device = request.device;
    nvrhi::BufferDesc desc;
    desc.byteSize = byteCount;
    desc.cpuAccess = nvrhi::CpuAccessMode::Read;
    desc.initialState = nvrhi::ResourceStates::CopyDest;
    desc.keepInitialState = true;
    desc.debugName = "Capture readback";
    nvrhi::BufferHandle readback = device->createBuffer(desc);
    if (!readback)
    {
        throw std::runtime_error("Failed to create the readback buffer");
    }
    RunNow(device, [&](nvrhi::ICommandList* commandList)
           {
               commandList->copyBuffer(readback, 0, source, 0, byteCount);
           });
    const void* mapped = device->mapBuffer(readback, nvrhi::CpuAccessMode::Read);
    if (mapped == nullptr)
    {
        throw std::runtime_error("Failed to map the readback buffer");
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(byteCount));
    std::memcpy(bytes.data(), mapped, bytes.size());
    device->unmapBuffer(readback);
    return bytes;
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
    const std::vector<uint8_t> pixels = ReadImageRgba8(request);
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

std::vector<uint8_t> ReadImageRgba8(const ImageCaptureRequest& request)
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
    return pixels;
}
}
