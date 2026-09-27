#pragma once

#include "common.h"
#include "uniform_buffer.h"

#include <engine/renderer/ddgi_volume.h>

#include <cstdint>
#include <span>
#include <vector>

namespace me
{

// The cascaded DDGI probes on the GPU (docs/design/2026-09-27-ddgi-design.md): the irradiance and
// visibility atlases (one array layer per level), the probe states, the ray buffer and each frame
// slot's schedule. Each frame the probes the CPU scheduled trace their rays (ddgi_trace.comp) and
// blend them into their tiles (ddgi_update.comp). Device lifetime like VulkanEnvironmentProbe: the
// images stay in GENERAL, set 0 binds them for every draw (bindings 21 to 23), and Record orders
// itself with its own barriers. It records after the ray scene and before the scene passes.
class VulkanDdgi
{
  public:
    // The most probes one frame can update (DdgiSettings::probesPerFrame is clamped to it).
    static constexpr uint32_t kMaxProbesPerFrame = 4096;

    VulkanDdgi(
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkPipelineCache pipelineCache,
        VkDescriptorSetLayout frameSetLayout,
        VkDescriptorSetLayout raySetLayout,
        uint32_t frameCount);
    ~VulkanDdgi();

    VulkanDdgi(const VulkanDdgi&) = delete;
    VulkanDdgi& operator=(const VulkanDdgi&) = delete;

    // This frame's schedule (PackDdgiProbe values, at most kMaxProbesPerFrame), into the slot's buffer.
    void SetSchedule(uint32_t frameSlot, std::span<const uint32_t> probes);

    // Clears every probe on the next Record: new content, whose light the old probes do not hold.
    void Invalidate();

    void Record(
        VkCommandBuffer commandBuffer,
        VkDescriptorSet frameSet,
        VkDescriptorSet raySet,
        uint32_t frameSlot,
        uint32_t frameIndex,
        float hysteresis);

    TextureDescriptorBinding GetIrradianceBinding() const;
    TextureDescriptorBinding GetVisibilityBinding() const;
    VkBuffer GetProbeStateBuffer() const;

  private:
    struct Image
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
    };

    Image CreateAtlas(uint32_t texelsPerProbe);
    Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
    void DestroyHandles();

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    uint32_t m_frameCount = 0;
    Image m_irradiance;
    Image m_visibility;
    VkSampler m_sampler = VK_NULL_HANDLE;
    Buffer m_states;
    Buffer m_rays;
    std::vector<Buffer> m_schedules;
    std::vector<uint32_t> m_scheduleCounts;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_sets;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_tracePipeline = VK_NULL_HANDLE;
    VkPipeline m_updatePipeline = VK_NULL_HANDLE;
    bool m_cleared = false;
};
}
