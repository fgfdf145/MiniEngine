#include "bloom_pass.h"

#include "nvrhi_pass.h"

#include <engine/renderer/bloom_chain.h>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace me
{

namespace
{
constexpr VkFormat kChainFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// Must match BloomConstants in shaders/vulkan/bloom.comp.
struct BloomPushConstants
{
    glm::uvec2 destinationExtent{0u};
    glm::vec2 sourceTexelSize{0.0f};
    uint32_t mode = 0;
    float unused[3] = {0.0f, 0.0f, 0.0f};
    // Upsample: level = destination * destinationWeight + tent(source) * sourceWeight. Composite:
    // scene = scene * destinationWeight + level 0 * sourceWeight. rgb used.
    glm::vec4 destinationWeight{1.0f};
    glm::vec4 sourceWeight{1.0f};
};
static_assert(sizeof(BloomPushConstants) == 64, "BloomPushConstants must match bloom.comp");

// Must match the MODE_* constants in bloom.comp.
constexpr uint32_t kModeFirstDownsample = 0u;
constexpr uint32_t kModeDownsample = 1u;
constexpr uint32_t kModeUpsample = 2u;
constexpr uint32_t kModeComposite = 3u;

// One level of the chain.
nvrhi::TextureSubresourceSet Level(size_t level)
{
    return nvrhi::TextureSubresourceSet(static_cast<nvrhi::MipLevel>(level), 1, 0, 1);
}
}

VulkanBloomPass::VulkanBloomPass(nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets, nvrhi::IBindingLayout* frameSetLayout)
    : m_nvrhiDevice(nvrhiDevice)
{
    m_sampler = CreateClampSampler(nvrhiDevice, VK_FILTER_LINEAR);
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 1;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = ShaderBindingOffsets();
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::Texture_SRV(0),
        nvrhi::BindingLayoutItem::Sampler(64),
        nvrhi::BindingLayoutItem::Texture_UAV(1),
        nvrhi::BindingLayoutItem::PushConstants(0, sizeof(BloomPushConstants))};
    m_setLayout = CreateNvrhiBindingLayout(m_nvrhiDevice, layoutDesc, "Failed to create the bloom binding layout");
    m_pipeline = CreateNvrhiComputePipeline(m_nvrhiDevice, "bloom.comp.spv", {frameSetLayout, m_setLayout});
    CreateChain(targets.GetOutputExtent());
    CreateBindingSets(targets);
}

VulkanBloomPass::~VulkanBloomPass() = default;

ScenePassId VulkanBloomPass::Id() const
{
    return ScenePassId::Bloom;
}

RenderPassIo VulkanBloomPass::Io() const
{
    // SceneTaa is read and written in place, in GENERAL, which a write declaration puts it in and
    // orders after TAA's store.
    static constexpr std::array<RenderTargetId, 1> kWrites = {RenderTargetId::SceneTaa};
    RenderPassIo io{};
    io.writes = kWrites;
    return io;
}

void VulkanBloomPass::Record(
    VkCommandBuffer commandBuffer,
    const SceneRenderTargets& targets,
    const ScenePassFrameContext& frame) const
{
    (void)commandBuffer;
    if (!frame.bloom.enabled)
    {
        return;
    }

    const uint32_t slot = targets.ResolveIndex(RenderTargetId::SceneTaa, frame.imageIndex, frame.frameSlot);
    nvrhi::ITexture* scene = targets.GetTexture(RenderTargetId::SceneTaa, slot);
    nvrhi::ICommandList* commandList = frame.commandList;
    const NvrhiPassScope scope(commandList, {{scene, nvrhi::ResourceStates::UnorderedAccess}});

    const glm::uvec2 sceneExtent(frame.outputExtent.width, frame.outputExtent.height);
    // Each level's share of every pixel's energy (see ComputeGlareBands). The upsample chain sums
    // band_k * blur_k into level 0, and the composite keeps 1 - total where it was.
    const size_t levelCount = m_levelExtents.size();
    const std::vector<glm::vec3> bands =
        ComputeGlareBands(
            frame.glareFNumber,
            frame.glareImageHeight != 0 ? frame.glareImageHeight : frame.outputExtent.height,
            levelCount,
            std::max(frame.bloom.strength, 0.0f));
    glm::vec3 total(0.0f);
    for (const glm::vec3& band : bands)
    {
        total += band;
    }

    // Each dispatch reads one image (a level, or SceneTaa) and writes another; the states set before
    // it order it after the dispatch that wrote what it reads, and its writes after earlier reads.
    BloomPushConstants constants{};
    const auto dispatch = [&](nvrhi::IBindingSet* set,
                              uint32_t mode,
                              nvrhi::ITexture* sourceTexture,
                              nvrhi::TextureSubresourceSet sourceLevel,
                              nvrhi::ITexture* destinationTexture,
                              nvrhi::TextureSubresourceSet destinationLevel,
                              glm::uvec2 source,
                              glm::uvec2 destination,
                              glm::vec3 destinationWeight,
                              glm::vec3 sourceWeight)
    {
        commandList->setTextureState(sourceTexture, sourceLevel, nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(destinationTexture, destinationLevel, nvrhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();
        nvrhi::ComputeState state;
        state.pipeline = m_pipeline;
        state.bindings = {frame.frameBindingSet, set};
        commandList->setComputeState(state);
        constants.mode = mode;
        constants.destinationWeight = glm::vec4(destinationWeight, 0.0f);
        constants.sourceWeight = glm::vec4(sourceWeight, 0.0f);
        constants.sourceTexelSize = 1.0f / glm::vec2(source);
        constants.destinationExtent = destination;
        commandList->setPushConstants(&constants, sizeof(constants));
        commandList->dispatch(
            (destination.x + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize,
            (destination.y + kComputeWorkgroupSize - 1) / kComputeWorkgroupSize);
    };

    const glm::vec3 one(1.0f);
    nvrhi::ITexture* chain = m_chainTexture;
    const nvrhi::TextureSubresourceSet whole = nvrhi::AllSubresources;
    dispatch(m_firstDownsampleSets.at(slot), kModeFirstDownsample, scene, whole, chain, Level(0), sceneExtent, m_levelExtents[0], one, one);
    for (size_t level = 1; level < levelCount; ++level)
    {
        dispatch(
            m_downsampleSets[level - 1],
            kModeDownsample,
            chain,
            Level(level - 1),
            chain,
            Level(level),
            m_levelExtents[level - 1],
            m_levelExtents[level],
            one,
            one);
    }
    // The bottom level is weighted as the first upsample reads it; every level above it is already
    // a weighted sum when the next one up reads it.
    for (size_t level = levelCount - 1; level-- > 0;)
    {
        const glm::vec3 sourceWeight = level + 2 == levelCount ? bands[level + 1] : one;
        dispatch(
            m_upsampleSets[level],
            kModeUpsample,
            chain,
            Level(level + 1),
            chain,
            Level(level),
            m_levelExtents[level + 1],
            m_levelExtents[level],
            bands[level],
            sourceWeight);
    }
    // With a single level no upsample ran, so level 0 is still unweighted.
    const glm::vec3 levelZeroWeight = levelCount == 1 ? bands[0] : one;
    dispatch(m_compositeSets.at(slot), kModeComposite, chain, Level(0), scene, whole, m_levelExtents[0], sceneExtent, one - total, levelZeroWeight);
}

void VulkanBloomPass::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateChain(targets.GetOutputExtent());
    CreateBindingSets(targets);
}

void VulkanBloomPass::CreateChain(VkExtent2D extent)
{
    // The binding sets name the old chain: they go with it.
    m_firstDownsampleSets.clear();
    m_compositeSets.clear();
    m_downsampleSets.clear();
    m_upsampleSets.clear();
    m_chainTexture = nullptr;
    m_levelExtents = BuildBloomMipChain(glm::uvec2(extent.width, extent.height));

    nvrhi::TextureDesc desc;
    desc.dimension = nvrhi::TextureDimension::Texture2D;
    desc.width = m_levelExtents[0].x;
    desc.height = m_levelExtents[0].y;
    desc.mipLevels = static_cast<uint32_t>(m_levelExtents.size());
    desc.format = ToNvrhiFormat(kChainFormat);
    desc.isShaderResource = true;
    desc.isUAV = true;
    desc.debugName = "Bloom chain";
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    m_chainTexture = m_nvrhiDevice->createTexture(desc);
    if (!m_chainTexture)
    {
        throw std::runtime_error("Failed to create the bloom chain");
    }
}

void VulkanBloomPass::CreateBindingSets(const SceneRenderTargets& targets)
{
    // Each read and each write names one level: a storage view may name only one level, and sampling
    // one level through its own view keeps the downsample from reading a level it is writing.
    const auto bindingSet = [&](nvrhi::ITexture* source,
                                nvrhi::TextureSubresourceSet sourceLevel,
                                nvrhi::ITexture* destination,
                                nvrhi::TextureSubresourceSet destinationLevel)
    {
        nvrhi::BindingSetDesc desc;
        desc.bindings = {
            nvrhi::BindingSetItem::Texture_SRV(0, source, nvrhi::Format::UNKNOWN, sourceLevel),
            nvrhi::BindingSetItem::Sampler(64, m_sampler),
            nvrhi::BindingSetItem::Texture_UAV(1, destination, nvrhi::Format::UNKNOWN, destinationLevel),
            nvrhi::BindingSetItem::PushConstants(0, sizeof(BloomPushConstants))};
        return CreateNvrhiBindingSet(m_nvrhiDevice, desc, m_setLayout, "Failed to create a bloom binding set");
    };

    const uint32_t copyCount = targets.GetTransientCopyCount();
    const size_t levelCount = m_levelExtents.size();
    const nvrhi::TextureSubresourceSet whole = nvrhi::AllSubresources;
    m_firstDownsampleSets.clear();
    m_compositeSets.clear();
    m_downsampleSets.clear();
    m_upsampleSets.clear();
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        nvrhi::ITexture* scene = targets.GetTexture(RenderTargetId::SceneTaa, slot);
        m_firstDownsampleSets.push_back(bindingSet(scene, whole, m_chainTexture, Level(0)));
        m_compositeSets.push_back(bindingSet(m_chainTexture, Level(0), scene, whole));
    }
    for (size_t level = 1; level < levelCount; ++level)
    {
        m_downsampleSets.push_back(bindingSet(m_chainTexture, Level(level - 1), m_chainTexture, Level(level)));
        m_upsampleSets.push_back(bindingSet(m_chainTexture, Level(level), m_chainTexture, Level(level - 1)));
    }
}
}
