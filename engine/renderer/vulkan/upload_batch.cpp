#include "upload_batch.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace me
{

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

VulkanUploadBatch::VulkanUploadBatch(VkPhysicalDevice physicalDevice, VkDevice device, uint32_t graphicsQueueFamily, VkQueue graphicsQueue)
    : VulkanUploadBatch(device, graphicsQueueFamily, graphicsQueue)
{
    m_physicalDevice = physicalDevice;
}

bool VulkanUploadBatch::CanStage() const
{
    return m_physicalDevice != VK_NULL_HANDLE;
}

VkDeviceSize VulkanUploadBatch::StagedBytes() const
{
    return m_stagedBytes;
}

VulkanUploadBatch::StagingSlice VulkanUploadBatch::Stage(const void* data, VkDeviceSize size, VkDeviceSize alignment)
{
    if (!CanStage())
    {
        throw std::logic_error("VulkanUploadBatch::Stage needs the batch made with a physical device");
    }
    const VkDeviceSize mask = std::max<VkDeviceSize>(alignment, 1) - 1;
    StagingChunk* chunk = m_stagingChunks.empty() ? nullptr : &m_stagingChunks.back();
    VkDeviceSize offset = chunk != nullptr ? (chunk->used + mask) & ~mask : 0;
    if ((chunk == nullptr || offset + size > chunk->size) && size <= kStagingChunkBytes && m_chunkPool != nullptr)
    {
        StagingChunk pooled;
        if (m_chunkPool->Take(pooled))
        {
            m_stagingChunks.push_back(pooled);
            chunk = &m_stagingChunks.back();
            offset = 0;
        }
    }
    if (chunk == nullptr || offset + size > chunk->size)
    {
        StagingChunk created;
        created.size = std::max(kStagingChunkBytes, size);
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = created.size;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        CheckVulkan(vkCreateBuffer(m_device, &bufferInfo, nullptr, &created.buffer), "Failed to create a staging chunk");
        // Tracked before anything else can throw, as TrackStagingResource asks.
        m_stagingChunks.push_back(created);
        StagingChunk& added = m_stagingChunks.back();
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(m_device, added.buffer, &requirements);
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &properties);
        constexpr VkMemoryPropertyFlags kWanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t typeIndex = UINT32_MAX;
        for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
        {
            if ((requirements.memoryTypeBits & (1u << index)) != 0 && (properties.memoryTypes[index].propertyFlags & kWanted) == kWanted)
            {
                typeIndex = index;
                break;
            }
        }
        if (typeIndex == UINT32_MAX)
        {
            throw std::runtime_error("No host-visible memory for staging");
        }
        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = typeIndex;
        CheckVulkan(vkAllocateMemory(m_device, &allocateInfo, nullptr, &added.memory), "Failed to allocate a staging chunk");
        CheckVulkan(vkBindBufferMemory(m_device, added.buffer, added.memory, 0), "Failed to bind a staging chunk");
        void* mapped = nullptr;
        CheckVulkan(vkMapMemory(m_device, added.memory, 0, added.size, 0, &mapped), "Failed to map a staging chunk");
        added.mapped = static_cast<unsigned char*>(mapped);
        chunk = &added;
        offset = 0;
    }
    std::memcpy(chunk->mapped + offset, data, static_cast<size_t>(size));
    chunk->used = offset + size;
    m_stagedBytes += size;
    return StagingSlice{chunk->buffer, offset};
}

void VulkanUploadBatch::ReleaseStagingChunks()
{
    for (const StagingChunk& chunk : m_stagingChunks)
    {
        if (m_chunkPool != nullptr && chunk.size == kStagingChunkBytes && chunk.mapped != nullptr && m_chunkPool->Give(chunk))
        {
            continue;
        }
        if (chunk.mapped != nullptr)
        {
            vkUnmapMemory(m_device, chunk.memory);
        }
        vkDestroyBuffer(m_device, chunk.buffer, nullptr);
        vkFreeMemory(m_device, chunk.memory, nullptr);
    }
    m_stagingChunks.clear();
    m_stagedBytes = 0;
}

VulkanUploadBatch::~VulkanUploadBatch()
{
    if (m_fence != VK_NULL_HANDLE)
    {
        // Submitted without a wait: its staging is read until the fence signals.
        vkWaitForFences(m_device, 1, &m_fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(m_device, m_fence, nullptr);
    }
    ReleaseStagingChunks();
    // Only reached with resources still tracked when an upload was abandoned without a final
    // Flush(). Their copies were recorded but never submitted, so nothing on the GPU reads them.
    for (const auto& [stagingBuffer, stagingMemory] : m_stagingResources)
    {
        vkDestroyBuffer(m_device, stagingBuffer, nullptr);
        vkFreeMemory(m_device, stagingMemory, nullptr);
    }

    if (m_commandPool != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
    }
}

VkCommandBuffer VulkanUploadBatch::GetCommandBuffer()
{
    m_hasCommands = true;
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
    if (!m_hasCommands && m_stagingResources.empty() && m_stagingChunks.empty())
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
    ReleaseStagingChunks();
    m_hasCommands = false;

    CheckVulkan(vkResetCommandPool(m_device, m_commandPool, 0), "Failed to reset upload batch command pool");
    BeginRecording();
}

void VulkanUploadBatch::SubmitWithoutWait()
{
    if (m_fence != VK_NULL_HANDLE)
    {
        throw std::logic_error("A VulkanUploadBatch is submitted without a wait only once");
    }
    // The uploads' own barriers name the stages they expect (a texture's, the fragment shader), and
    // Flush's wait covered the rest; here every later command, of any stage, waits for the copies and
    // layout changes and sees what they wrote.
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(
        m_commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    CheckVulkan(vkEndCommandBuffer(m_commandBuffer), "Failed to end upload batch command buffer");

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    CheckVulkan(vkCreateFence(m_device, &fenceInfo, nullptr, &m_fence), "Failed to create an upload batch fence");
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &m_commandBuffer;
    const VkResult submitted = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_fence);
    if (submitted != VK_SUCCESS)
    {
        // Nothing reached the GPU: the destructor must not wait for it.
        vkDestroyFence(m_device, m_fence, nullptr);
        m_fence = VK_NULL_HANDLE;
        CheckVulkan(submitted, "Failed to submit upload batch");
    }
    m_hasCommands = false;
}

bool VulkanUploadBatch::IsComplete() const
{
    return m_fence == VK_NULL_HANDLE || vkGetFenceStatus(m_device, m_fence) == VK_SUCCESS;
}

bool VulkanUploadBatch::IsEmpty() const
{
    return !m_hasCommands && m_stagingResources.empty() && m_stagingChunks.empty();
}

void VulkanUploadBatch::SetChunkPool(VulkanStagingChunkPool* pool)
{
    m_chunkPool = pool;
}

VulkanStagingChunkPool::VulkanStagingChunkPool(VkDevice device)
    : m_device(device)
{
}

VulkanStagingChunkPool::~VulkanStagingChunkPool()
{
    for (const VulkanStagingChunk& chunk : m_chunks)
    {
        vkUnmapMemory(m_device, chunk.memory);
        vkDestroyBuffer(m_device, chunk.buffer, nullptr);
        vkFreeMemory(m_device, chunk.memory, nullptr);
    }
}

bool VulkanStagingChunkPool::Take(VulkanStagingChunk& chunk)
{
    const std::lock_guard lock(m_mutex);
    if (m_chunks.empty())
    {
        return false;
    }
    chunk = m_chunks.back();
    chunk.used = 0;
    m_chunks.pop_back();
    return true;
}

bool VulkanStagingChunkPool::Give(const VulkanStagingChunk& chunk)
{
    const std::lock_guard lock(m_mutex);
    if (m_chunks.size() >= kMaxChunks)
    {
        return false;
    }
    m_chunks.push_back(chunk);
    return true;
}
}
