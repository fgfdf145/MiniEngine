#include "upload_batch.h"

VulkanUploadBatch::VulkanUploadBatch(VkDevice device, uint32_t graphicsQueueFamily, VkQueue graphicsQueue)
    : m_device(device), m_graphicsQueue(graphicsQueue)
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = graphicsQueueFamily;
    CheckVulkan(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool), "Failed to create upload batch command pool");

    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = m_commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    CheckVulkan(vkAllocateCommandBuffers(m_device, &allocateInfo, &m_commandBuffer), "Failed to allocate upload batch command buffer");

    BeginRecording();
}

VulkanUploadBatch::~VulkanUploadBatch()
{
    if (m_commandPool != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
    }
}

VkCommandBuffer VulkanUploadBatch::GetCommandBuffer() const
{
    return m_commandBuffer;
}

void VulkanUploadBatch::TrackStagingResource(VkBuffer buffer, VkDeviceMemory memory)
{
    m_stagingResources.emplace_back(buffer, memory);
}

void VulkanUploadBatch::BeginRecording()
{
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CheckVulkan(vkBeginCommandBuffer(m_commandBuffer, &beginInfo), "Failed to begin upload batch command buffer");
}

void VulkanUploadBatch::Flush()
{
    if (m_stagingResources.empty())
    {
        return;
    }

    // One submit + one wait for everything recorded since the last Flush(), instead of a
    // submit-and-stall per resource. This is what makes loading models with hundreds of
    // submeshes/textures (e.g. Sponza) fast: per-resource vkQueueWaitIdle serializes the
    // whole upload into one GPU round-trip after another.
    CheckVulkan(vkEndCommandBuffer(m_commandBuffer), "Failed to end upload batch command buffer");

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_commandBuffer;
    CheckVulkan(vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE), "Failed to submit upload batch");
    CheckVulkan(vkQueueWaitIdle(m_graphicsQueue), "Failed to wait for upload batch");

    for (const auto& [stagingBuffer, stagingMemory] : m_stagingResources)
    {
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
    }
    m_stagingResources.clear();

    CheckVulkan(vkResetCommandPool(m_device, m_commandPool, 0), "Failed to reset upload batch command pool");
    BeginRecording();
}
