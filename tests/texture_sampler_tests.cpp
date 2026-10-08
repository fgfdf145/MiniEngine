#include <engine/renderer/vulkan/sampler_settings.h>
#include <engine/scene/material_graph.h>

#include <iostream>
#include <set>
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
    const nvrhi::SamplerDesc desc = BuildTextureSamplerDesc(TextureSampler{}, 16.0f);
    Require(desc.magFilter && desc.minFilter, "linear filtering");
    Require(desc.mipFilter, "linear mipmaps");
    Require(desc.addressU == nvrhi::SamplerAddressMode::Repeat && desc.addressV == nvrhi::SamplerAddressMode::Repeat,
            "repeat on both axes");
    Require(desc.maxAnisotropy == 16.0f, "16x anisotropy");
    Require(desc.minLod == 0.0f && desc.maxLod == VK_LOD_CLAMP_NONE, "every mip level");
    Require(BuildTextureSamplerDesc(TextureSampler{}, 0.0f).maxAnisotropy == 1.0f, "no anisotropy on a device without it");
}

void MapsSettingsToSamplers()
{
    TextureSampler sampler;
    sampler.wrapS = TextureWrap::ClampToEdge;
    sampler.wrapT = TextureWrap::MirroredRepeat;
    sampler.magFilter = TextureFilter::Nearest;
    sampler.minFilter = TextureFilter::Nearest;
    sampler.mipFilter = TextureMipFilter::None;
    const nvrhi::SamplerDesc desc = BuildTextureSamplerDesc(sampler, 16.0f);
    Require(desc.addressU == nvrhi::SamplerAddressMode::Clamp, "wrapS is U");
    Require(desc.addressV == nvrhi::SamplerAddressMode::Mirror, "wrapT is V");
    Require(!desc.magFilter && !desc.minFilter, "nearest filtering");
    Require(!desc.mipFilter && desc.maxLod == 0.25f, "no mipmaps samples the base level only");
    Require(desc.maxAnisotropy == 1.0f, "no anisotropy with nearest filtering");

    TextureSampler nearestMips;
    nearestMips.mipFilter = TextureMipFilter::Nearest;
    const nvrhi::SamplerDesc mips = BuildTextureSamplerDesc(nearestMips, 16.0f);
    Require(!mips.mipFilter && mips.maxLod == VK_LOD_CLAMP_NONE, "nearest mipmaps");
    Require(mips.maxAnisotropy == 16.0f, "linear filters with mipmaps keep anisotropy");
}

void ClampSamplerReadsTheBaseLevel()
{
    for (const bool linear : {false, true})
    {
        const nvrhi::SamplerDesc desc = BuildClampSamplerDesc(linear);
        Require(desc.magFilter == linear && desc.minFilter == linear, "nearest or linear as asked");
        Require(!desc.mipFilter && desc.minLod == 0.0f && desc.maxLod == 0.0f, "the base level alone");
        Require(desc.addressU == nvrhi::SamplerAddressMode::Clamp && desc.addressV == nvrhi::SamplerAddressMode::Clamp &&
                    desc.addressW == nvrhi::SamplerAddressMode::Clamp,
                "clamped to the edge");
        Require(desc.maxAnisotropy == 1.0f && desc.reductionType == nvrhi::SamplerReductionType::Standard,
                "no anisotropy, no comparison");
    }
}
void SamplerIndicesCoverEverySampler()
{
    Require(VulkanSamplerCache::SamplerAt(0).IsDefault(), "index 0 is the default sampler");
    std::set<std::tuple<int, int, int, int, int>> seen;
    for (uint32_t index = 0; index < VulkanSamplerCache::kSamplerCount; ++index)
    {
        const TextureSampler sampler = VulkanSamplerCache::SamplerAt(index);
        Require(static_cast<int>(sampler.wrapS) <= 2 && static_cast<int>(sampler.wrapT) <= 2 && static_cast<int>(sampler.mipFilter) <= 2,
                "fields within their enums");
        seen.insert({static_cast<int>(sampler.wrapS), static_cast<int>(sampler.wrapT), static_cast<int>(sampler.magFilter),
                     static_cast<int>(sampler.minFilter), static_cast<int>(sampler.mipFilter)});
    }
    Require(seen.size() == VulkanSamplerCache::kSamplerCount, "every index a different sampler");
    bool threw = false;
    try
    {
        VulkanSamplerCache::SamplerAt(VulkanSamplerCache::kSamplerCount);
    }
    catch (const std::out_of_range&)
    {
        threw = true;
    }
    Require(threw, "no sampler past the count");
}
}

int main()
{
    try
    {
        ReadsGltfWrapModes();
        ReadsGltfFilters();
        DefaultIsTodaysSampler();
        MapsSettingsToSamplers();
        ClampSamplerReadsTheBaseLevel();
        SamplerIndicesCoverEverySampler();
    }
    catch (const std::exception& error)
    {
        std::cerr << "texture sampler tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "texture sampler tests passed\n";
    return 0;
}
