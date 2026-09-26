#include "texture_loader.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <tinyexr.h>

#if MINIENGINE_HAS_KTX
#include <ktx.h>
#endif

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace me
{

namespace
{
std::string LowerExtension(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });
    return ext;
}

std::vector<std::uint8_t> ReadFileBytes(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("Failed to open texture '" + path + "'");
    }
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool IsPortableMapExtension(const std::string& path)
{
    const std::string ext = LowerExtension(path);
    return ext == ".ppm" || ext == ".pgm" || ext == ".pbm";
}

void FlipRows(TextureData& texture)
{
    const size_t rowSize = static_cast<size_t>(texture.width) * 4;
    std::vector<std::uint8_t> flipped(texture.pixels.size());
    for (int y = 0; y < texture.height; ++y)
    {
        const size_t sourceOffset = static_cast<size_t>(y) * rowSize;
        const size_t destinationOffset = static_cast<size_t>(texture.height - 1 - y) * rowSize;
        std::memcpy(flipped.data() + destinationOffset, texture.pixels.data() + sourceOffset, rowSize);
    }
    texture.pixels = std::move(flipped);
}

FloatTextureData LoadRadianceHdr(const std::string& path)
{
    // stbi_loadf also accepts LDR files and converts them with a gamma curve, which would make a PNG
    // renamed to .hdr load as something it is not.
    if (!stbi_is_hdr(path.c_str()))
    {
        throw std::runtime_error("'" + path + "' is not a Radiance HDR image");
    }
    int width = 0;
    int height = 0;
    int channelCount = 0;
    float* rawPixels = stbi_loadf(path.c_str(), &width, &height, &channelCount, STBI_rgb_alpha);
    if (rawPixels == nullptr)
    {
        const char* reason = stbi_failure_reason();
        throw std::runtime_error("Failed to load texture '" + path + "': " + (reason ? reason : "unknown error"));
    }
    FloatTextureData texture{};
    texture.width = width;
    texture.height = height;
    texture.pixels.assign(rawPixels, rawPixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    stbi_image_free(rawPixels);
    return texture;
}

FloatTextureData LoadOpenExr(const std::string& path)
{
    float* rawPixels = nullptr;
    int width = 0;
    int height = 0;
    const char* error = nullptr;
    const int result = LoadEXR(&rawPixels, &width, &height, path.c_str(), &error);
    if (result != TINYEXR_SUCCESS)
    {
        const std::string reason = error != nullptr ? error : "error code " + std::to_string(result);
        FreeEXRErrorMessage(error);
        throw std::runtime_error("Failed to load texture '" + path + "': " + reason);
    }
    FloatTextureData texture{};
    texture.width = width;
    texture.height = height;
    texture.pixels.assign(rawPixels, rawPixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    std::free(rawPixels);
    return texture;
}

// NaN fails the comparison and ends up 0.
float Saturate(float value)
{
    return value > 0.0f ? std::min(value, 1.0f) : 0.0f;
}

float LinearToSrgb(float value)
{
    return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

// What a float image looks like through the RGBA8 path: clamped to [0, 1], colour sRGB-encoded.
TextureData ToDisplayRgba8(const FloatTextureData& image)
{
    TextureData texture{};
    texture.width = image.width;
    texture.height = image.height;
    texture.channelCount = 4;
    texture.pixels.resize(image.pixels.size());
    for (size_t index = 0; index < image.pixels.size(); ++index)
    {
        const float value = Saturate(image.pixels[index]);
        const bool isAlpha = index % 4 == 3;
        texture.pixels[index] = static_cast<std::uint8_t>(std::lround((isAlpha ? value : LinearToSrgb(value)) * 255.0f));
    }
    return texture;
}

std::string ReadNextPortableMapToken(std::istream& input)
{
    std::string token;

    while (input >> token)
    {
        if (!token.empty() && token[0] == '#')
        {
            std::string comment;
            std::getline(input, comment);
            continue;
        }

        return token;
    }

    throw std::runtime_error("Unexpected end of portable pixmap file");
}

TextureData LoadPortablePixmap(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        throw std::runtime_error("Failed to open portable pixmap: " + path);
    }

    const std::string magic = ReadNextPortableMapToken(input);
    if (magic != "P3")
    {
        throw std::runtime_error("Unsupported portable pixmap format: " + magic);
    }

    const int width = std::stoi(ReadNextPortableMapToken(input));
    const int height = std::stoi(ReadNextPortableMapToken(input));
    const int maxValue = std::stoi(ReadNextPortableMapToken(input));
    if (width <= 0 || height <= 0 || maxValue <= 0)
    {
        throw std::runtime_error("Invalid portable pixmap header");
    }

    TextureData texture{};
    texture.width = width;
    texture.height = height;
    texture.channelCount = 4;
    texture.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    const float scale = 255.0f / static_cast<float>(maxValue);
    for (int i = 0; i < width * height; ++i)
    {
        const int red = std::stoi(ReadNextPortableMapToken(input));
        const int green = std::stoi(ReadNextPortableMapToken(input));
        const int blue = std::stoi(ReadNextPortableMapToken(input));

        texture.pixels[static_cast<size_t>(i) * 4 + 0] = static_cast<std::uint8_t>(red * scale);
        texture.pixels[static_cast<size_t>(i) * 4 + 1] = static_cast<std::uint8_t>(green * scale);
        texture.pixels[static_cast<size_t>(i) * 4 + 2] = static_cast<std::uint8_t>(blue * scale);
        texture.pixels[static_cast<size_t>(i) * 4 + 3] = 255;
    }

    return texture;
}
}

bool TextureLoader::IsFloatImageFile(const std::filesystem::path& path)
{
    const std::string ext = LowerExtension(path);
    return ext == ".hdr" || ext == ".exr";
}

FloatTextureData TextureLoader::LoadRGBA32F(const std::string& path)
{
    if (!IsFloatImageFile(path))
    {
        throw std::runtime_error("'" + path + "' is not a floating-point image (.hdr or .exr)");
    }
    return LowerExtension(path) == ".exr" ? LoadOpenExr(path) : LoadRadianceHdr(path);
}

std::uint16_t PackHalfFloat(float value)
{
    if (std::isnan(value))
    {
        return 0;
    }
    constexpr float kHalfMax = 65504.0f;
    return glm::packHalf1x16(std::clamp(value, -kHalfMax, kHalfMax));
}

HalfFloatTextureData PackRgba16Float(const FloatTextureData& image)
{
    HalfFloatTextureData packed{};
    packed.width = image.width;
    packed.height = image.height;
    packed.texels.resize(image.pixels.size());
    std::transform(image.pixels.begin(), image.pixels.end(), packed.texels.begin(), PackHalfFloat);
    return packed;
}

bool TextureLoader::IsKtx2(const std::uint8_t* bytes, size_t size)
{
    static constexpr std::uint8_t kIdentifier[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    return bytes != nullptr && size >= sizeof(kIdentifier) && std::memcmp(bytes, kIdentifier, sizeof(kIdentifier)) == 0;
}

TextureData TextureLoader::DecodeKtx2(const std::uint8_t* bytes, size_t size, const std::string& source)
{
#if MINIENGINE_HAS_KTX
    ktxTexture2* texture = nullptr;
    KTX_error_code result = ktxTexture2_CreateFromMemory(bytes, size, KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &texture);
    if (result != KTX_SUCCESS)
    {
        throw std::runtime_error("Failed to load KTX2 texture '" + source + "': " + ktxErrorString(result));
    }
    struct Destroy
    {
        ktxTexture2* texture;
        ~Destroy()
        {
            ktxTexture_Destroy(ktxTexture(texture));
        }
    } destroy{texture};

    if (ktxTexture2_NeedsTranscoding(texture))
    {
        result = ktxTexture2_TranscodeBasis(texture, KTX_TTF_RGBA32, 0);
        if (result != KTX_SUCCESS)
        {
            throw std::runtime_error("Failed to transcode KTX2 texture '" + source + "': " + ktxErrorString(result));
        }
    }
    // VK_FORMAT_R8G8B8A8_UNORM and VK_FORMAT_R8G8B8A8_SRGB: what Basis transcodes to, and the one
    // uncompressed layout read directly. Whether the texels are sRGB is the material slot's call.
    constexpr ktx_uint32_t kRgba8Unorm = 37;
    constexpr ktx_uint32_t kRgba8Srgb = 43;
    if (texture->vkFormat != kRgba8Unorm && texture->vkFormat != kRgba8Srgb)
    {
        throw std::runtime_error(
            "KTX2 texture '" + source + "' has VkFormat " + std::to_string(texture->vkFormat) +
            "; only Basis Universal and 8-bit RGBA KTX2 textures are supported");
    }

    ktx_size_t offset = 0;
    result = ktxTexture_GetImageOffset(ktxTexture(texture), 0, 0, 0, &offset);
    if (result != KTX_SUCCESS)
    {
        throw std::runtime_error("KTX2 texture '" + source + "' has no base level: " + ktxErrorString(result));
    }
    TextureData decoded{};
    decoded.width = static_cast<int>(texture->baseWidth);
    decoded.height = static_cast<int>(texture->baseHeight);
    decoded.channelCount = 4;
    const size_t rowSize = static_cast<size_t>(decoded.width) * 4;
    const size_t rowPitch = ktxTexture_GetRowPitch(ktxTexture(texture), 0);
    const ktx_uint8_t* data = ktxTexture_GetData(ktxTexture(texture)) + offset;
    if (rowPitch < rowSize || offset + rowPitch * static_cast<size_t>(decoded.height) > ktxTexture_GetDataSize(ktxTexture(texture)))
    {
        throw std::runtime_error("KTX2 texture '" + source + "' has a base level smaller than its size");
    }
    decoded.pixels.resize(rowSize * static_cast<size_t>(decoded.height));
    for (int row = 0; row < decoded.height; ++row)
    {
        std::memcpy(decoded.pixels.data() + static_cast<size_t>(row) * rowSize, data + static_cast<size_t>(row) * rowPitch, rowSize);
    }
    return decoded;
#else
    (void)bytes;
    (void)size;
    throw std::runtime_error("KTX2 texture '" + source + "' cannot be loaded: this build has no libktx");
#endif
}

TextureData TextureLoader::LoadRGBA8(const std::string& path, bool flipVertically)
{
    if (LowerExtension(path) == ".ktx2")
    {
        const std::vector<std::uint8_t> bytes = ReadFileBytes(path);
        TextureData texture = DecodeKtx2(bytes.data(), bytes.size(), path);
        if (flipVertically)
        {
            FlipRows(texture);
        }
        return texture;
    }

    if (IsFloatImageFile(path))
    {
        TextureData texture = ToDisplayRgba8(LoadRGBA32F(path));
        if (flipVertically)
        {
            FlipRows(texture);
        }
        return texture;
    }

    stbi_set_flip_vertically_on_load(flipVertically ? 1 : 0);

    int width = 0;
    int height = 0;
    int channelCount = 0;
    stbi_uc* rawPixels = stbi_load(path.c_str(), &width, &height, &channelCount, STBI_rgb_alpha);
    if (rawPixels == nullptr)
    {
        if (!IsPortableMapExtension(path))
        {
            const char* reason = stbi_failure_reason();
            throw std::runtime_error(
                "Failed to load texture '" + path + "': " +
                (reason ? reason : "unknown error"));
        }

        TextureData texture = LoadPortablePixmap(path);
        if (flipVertically)
        {
            FlipRows(texture);
        }
        return texture;
    }

    TextureData texture{};
    texture.width = width;
    texture.height = height;
    texture.channelCount = 4;
    texture.pixels.assign(rawPixels, rawPixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);

    stbi_image_free(rawPixels);
    return texture;
}
}
