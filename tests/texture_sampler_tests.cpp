#include <engine/renderer/vulkan/sampler_settings.h>
#include <engine/scene/material_graph.h>

#include <iostream>
#include <stdexcept>
#include <string>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

constexpr int kUnset = -1;

void ReadsGltfWrapModes()
{
    const TextureSampler sampler = TextureSamplerFromGltf(33071, 33648, kUnset, kUnset);
    Require(sampler.wrapS == TextureWrap::ClampToEdge, "33071 is CLAMP_TO_EDGE");
    Require(sampler.wrapT == TextureWrap::MirroredRepeat, "33648 is MIRRORED_REPEAT");
    Require(TextureSamplerFromGltf(10497, 10497, kUnset, kUnset).IsDefault(), "REPEAT on both axes is the default");
    Require(TextureSamplerFromGltf(kUnset, 12345, kUnset, kUnset).IsDefault(), "unset and unknown wrap modes repeat");
}

void ReadsGltfFilters()
{
    struct MinCase
    {
        int gltf;
        TextureFilter filter;
        TextureMipFilter mip;
    };
    const MinCase cases[] = {
        {9728, TextureFilter::Nearest, TextureMipFilter::None},
        {9729, TextureFilter::Linear, TextureMipFilter::None},
        {9984, TextureFilter::Nearest, TextureMipFilter::Nearest},
        {9985, TextureFilter::Linear, TextureMipFilter::Nearest},
        {9986, TextureFilter::Nearest, TextureMipFilter::Linear},
        {9987, TextureFilter::Linear, TextureMipFilter::Linear}};
    for (const MinCase& c : cases)
    {
        const TextureSampler sampler = TextureSamplerFromGltf(kUnset, kUnset, kUnset, c.gltf);
        Require(sampler.minFilter == c.filter && sampler.mipFilter == c.mip, "minFilter " + std::to_string(c.gltf));
    }
    Require(TextureSamplerFromGltf(kUnset, kUnset, 9728, kUnset).magFilter == TextureFilter::Nearest, "magFilter NEAREST");
    Require(TextureSamplerFromGltf(kUnset, kUnset, 9729, kUnset).IsDefault(), "magFilter LINEAR is the default");
    Require(TextureSamplerFromGltf(kUnset, kUnset, 1, 2).IsDefault(), "unknown filters keep the default");
}

void DefaultIsTodaysSampler()
{
    const VkSamplerCreateInfo info = BuildTextureSamplerInfo(TextureSampler{}, 16.0f);
    Require(info.magFilter == VK_FILTER_LINEAR && info.minFilter == VK_FILTER_LINEAR, "linear filtering");
    Require(info.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR, "linear mipmaps");
    Require(info.addressModeU == VK_SAMPLER_ADDRESS_MODE_REPEAT && info.addressModeV == VK_SAMPLER_ADDRESS_MODE_REPEAT,
            "repeat on both axes");
    Require(info.anisotropyEnable == VK_TRUE && info.maxAnisotropy == 16.0f, "16x anisotropy");
    Require(info.minLod == 0.0f && info.maxLod == VK_LOD_CLAMP_NONE, "every mip level");
    Require(BuildTextureSamplerInfo(TextureSampler{}, 0.0f).anisotropyEnable == VK_FALSE, "no anisotropy on a device without it");
}

void MapsSettingsToVulkan()
{
    TextureSampler sampler;
    sampler.wrapS = TextureWrap::ClampToEdge;
    sampler.wrapT = TextureWrap::MirroredRepeat;
    sampler.magFilter = TextureFilter::Nearest;
    sampler.minFilter = TextureFilter::Nearest;
    sampler.mipFilter = TextureMipFilter::None;
    const VkSamplerCreateInfo info = BuildTextureSamplerInfo(sampler, 16.0f);
    Require(info.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, "wrapS is U");
    Require(info.addressModeV == VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, "wrapT is V");
    Require(info.magFilter == VK_FILTER_NEAREST && info.minFilter == VK_FILTER_NEAREST, "nearest filtering");
    Require(info.mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST && info.maxLod == 0.25f, "no mipmaps samples the base level only");
    Require(info.anisotropyEnable == VK_FALSE, "no anisotropy with nearest filtering");

    TextureSampler nearestMips;
    nearestMips.mipFilter = TextureMipFilter::Nearest;
    const VkSamplerCreateInfo mips = BuildTextureSamplerInfo(nearestMips, 16.0f);
    Require(mips.mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST && mips.maxLod == VK_LOD_CLAMP_NONE, "nearest mipmaps");
    Require(mips.anisotropyEnable == VK_TRUE, "linear filters with mipmaps keep anisotropy");
}
}

int main()
{
    try
    {
        ReadsGltfWrapModes();
        ReadsGltfFilters();
        DefaultIsTodaysSampler();
        MapsSettingsToVulkan();
    }
    catch (const std::exception& error)
    {
        std::cerr << "texture sampler tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "texture sampler tests passed\n";
    return 0;
}
