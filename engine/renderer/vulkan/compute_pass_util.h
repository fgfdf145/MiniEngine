#pragma once

#include "common.h"

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

VkSampler CreateClampSampler(VkDevice device, VkFilter filter);

// A memory type in typeFilter with every one of properties; throws when there is none.
uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);

// One binding per type, all compute stage, binding N of type types[N].
VkDescriptorSetLayout CreateComputeSetLayout(VkDevice device, std::span<const VkDescriptorType> types);

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

VkDescriptorPool CreateImageDescriptorPool(VkDevice device, uint32_t setCount, uint32_t samplersPerSet, uint32_t storagePerSet);

// Resets the pool, then allocates count sets of one layout from it.
std::vector<VkDescriptorSet> AllocateDescriptorSets(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout, uint32_t count);

VkWriteDescriptorSet ImageWrite(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* info);

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

    void Create(VkPhysicalDevice physicalDevice, VkDevice device, VkExtent2D extent, VkFormat format);
    void Destroy();

    VkImageView GetView(uint32_t index) const;

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
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    VkDevice m_device = VK_NULL_HANDLE;
    std::array<Image, 2> m_images{};
};
}
