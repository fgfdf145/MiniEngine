#pragma once

#include "common.h"
#include "nvrhi_native.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace me
{

// The plumbing the full-screen compute passes (the AO trace and resolve, TAA) share. Every Vulkan
// failure throws through CheckVulkan with a message naming the pass that asked.

// Must match local_size_x / local_size_y in every shader dispatched through DispatchCompute.
inline constexpr uint32_t kComputeWorkgroupSize = 8;

// BuildClampSamplerDesc's sampler: clamped, base level only, nearest or linear by filter.
nvrhi::SamplerHandle CreateClampSampler(nvrhi::IDevice* device, VkFilter filter);

// A memory type in typeFilter with every one of properties; throws when there is none.
uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);

// One binding per type, binding N of type types[N], visible to stages (the compute stage unless
// another shader reads the set too).
VkDescriptorSetLayout CreateComputeSetLayout(
    VkDevice device,
    std::span<const VkDescriptorType> types,
    VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT);

// A sampled texture's own sampler sits at the texture's binding plus this (the shaders' b + 64):
// the shaders take separate textures and samplers, which every target has, rather than combined
// image samplers, which DXIL does not.
inline constexpr uint32_t kSplitSamplerBindingOffset = 64;

// The same plus a VK_DESCRIPTOR_TYPE_SAMPLER binding at kSplitSamplerBindingOffset + b for each b in
// samplerBindings (bindings of sampled images the shader samples, not only loads).
VkDescriptorSetLayout CreateComputeSetLayout(
    VkDevice device,
    std::span<const VkDescriptorType> types,
    std::span<const uint32_t> samplerBindings,
    VkShaderStageFlags stages = VK_SHADER_STAGE_COMPUTE_BIT);

// Set 0 is the frame set, set 1 the pass's own; one compute-stage push constant range of
// pushConstantSize bytes.
void CreateComputePipeline(
    VkDevice device,
    VkPipelineCache pipelineCache,
    VkDescriptorSetLayout frameSetLayout,
    VkDescriptorSetLayout passSetLayout,
    const char* shaderName,
    uint32_t pushConstantSize,
    VkPipelineLayout& pipelineLayout,
    VkPipeline& pipeline);

// The same over any set layouts, in set order; no push constant range when pushConstantSize is 0.
void CreateComputePipeline(
    VkDevice device,
    VkPipelineCache pipelineCache,
    std::span<const VkDescriptorSetLayout> setLayouts,
    const char* shaderName,
    uint32_t pushConstantSize,
    VkPipelineLayout& pipelineLayout,
    VkPipeline& pipeline);

// A compute pipeline for a shader file under EnginePaths::ShaderRoot(), with an existing layout.
VkPipeline CreateComputeShaderPipeline(VkDevice device, VkPipelineCache pipelineCache, VkPipelineLayout pipelineLayout, const char* shaderName);

// Binds both sets, pushes the constants and covers extent with 8 x 8 workgroups.
void DispatchCompute(
    VkCommandBuffer commandBuffer,
    VkPipeline pipeline,
    VkPipelineLayout pipelineLayout,
    VkDescriptorSet frameSet,
    VkDescriptorSet passSet,
    const void* constants,
    uint32_t constantSize,
    VkExtent2D extent);

// Room for setCount sets of sampledPerSet sampled images, storagePerSet storage images and
// samplersPerSet separate samplers.
VkDescriptorPool CreateImageDescriptorPool(
    VkDevice device, uint32_t setCount, uint32_t sampledPerSet, uint32_t storagePerSet, uint32_t samplersPerSet = 0);

// Resets the pool, then allocates count sets of one layout from it.
std::vector<VkDescriptorSet> AllocateDescriptorSets(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout, uint32_t count);

VkWriteDescriptorSet ImageWrite(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* info);

// Every level and layer of a colour image from oldLayout to newLayout, srcAccess before dstAccess.
VkImageMemoryBarrier ColorImageTransition(
    VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags srcAccess, VkAccessFlags dstAccess);

// One barrier moving each colour image so, after srcStage and before dstStage. The images set 0
// samples rest in SHADER_READ_ONLY_OPTIMAL, the layout an NVRHI binding set names them in; their
// writers move them to GENERAL around the writes.
void TransitionColorImages(
    VkCommandBuffer commandBuffer,
    std::span<const VkImage> images,
    VkImageLayout oldLayout,
    VkImageLayout newLayout,
    VkPipelineStageFlags srcStage,
    VkAccessFlags srcAccess,
    VkPipelineStageFlags dstStage,
    VkAccessFlags dstAccess);

// Those images to GENERAL for a compute writer, after every shader's reads of them...
void BeginFrameImageWrites(VkCommandBuffer commandBuffer, std::span<const VkImage> images);

// ...and back to SHADER_READ_ONLY_OPTIMAL, srcStage's srcAccess visible to every shader after.
void EndFrameImageWrites(
    VkCommandBuffer commandBuffer,
    std::span<const VkImage> images,
    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VkAccessFlags srcAccess = VK_ACCESS_SHADER_WRITE_BIT);

// Two storage-and-sampled images a temporal filter ping-pongs between. They live outside
// SceneRenderTargets because they must survive across frames, which the frame-scoped layout tracker
// cannot describe, so they stay in VK_IMAGE_LAYOUT_GENERAL and RecordBarrier orders them itself.
class HistoryImagePair
{
  public:
    HistoryImagePair() = default;
    ~HistoryImagePair();

    HistoryImagePair(const HistoryImagePair&) = delete;
    HistoryImagePair& operator=(const HistoryImagePair&) = delete;

    // Storage and sampled, plus extraUsage (TAA's are copied into when DLSS writes the frame). The images
    // are NVRHI's, each with memory of its own.
    void Create(nvrhi::IDevice* nvrhiDevice, VkDevice device, VkExtent2D extent, VkFormat format, VkImageUsageFlags extraUsage = 0);
    void Destroy();

    VkImage GetImage(uint32_t index) const;
    VkImageView GetView(uint32_t index) const;
    nvrhi::ITexture* GetTexture(uint32_t index) const;

    // Both images, compute to compute. Invalid history is discarded with an UNDEFINED to GENERAL
    // transition, which is also the one a freshly created image needs; valid history keeps its
    // contents behind a GENERAL to GENERAL barrier that makes last frame's store visible and orders
    // this frame's store after last frame's sample. A barrier's first scope covers every command
    // submitted earlier on the queue, so this reaches across command buffers. Record it even when
    // the effect is off: the bound descriptors name both images in GENERAL.
    void RecordBarrier(VkCommandBuffer commandBuffer, bool historyValid) const;

  private:
    struct Image
    {
        nvrhi::TextureHandle texture;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    VkDevice m_device = VK_NULL_HANDLE;
    std::array<Image, 2> m_images{};
};
}
