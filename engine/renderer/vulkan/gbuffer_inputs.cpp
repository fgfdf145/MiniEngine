#include "gbuffer_inputs.h"

namespace me
{

VulkanGBufferDescriptors::VulkanGBufferDescriptors(VkDevice device, const SceneRenderTargets& targets)
    : m_device(device)
{
    try
    {
        CreateSetLayouts();
        CreateSampler();
        CreateDescriptorSets(targets);
    }
    catch (...)
    {
        DestroyHandles();
        throw;
    }
}

VulkanGBufferDescriptors::~VulkanGBufferDescriptors()
{
    DestroyHandles();
}

VkDescriptorSetLayout VulkanGBufferDescriptors::GetSetLayout() const
{
    return m_setLayout;
}

VkDescriptorSetLayout VulkanGBufferDescriptors::GetEmptySetLayout() const
{
    return m_emptySetLayout;
}

VkDescriptorSet VulkanGBufferDescriptors::GetSet(
    const SceneRenderTargets& targets,
    uint32_t imageIndex,
    uint32_t frameSlot) const
{
    return m_descriptorSets.at(targets.ResolveIndex(kInputs.front(), imageIndex, frameSlot));
}

void VulkanGBufferDescriptors::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateDescriptorSets(targets);
}

void VulkanGBufferDescriptors::CreateSetLayouts()
{
    std::array<VkDescriptorSetLayoutBinding, kInputs.size()> bindings{};
    for (uint32_t binding = 0; binding < static_cast<uint32_t>(bindings.size()); ++binding)
    {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    CheckVulkan(
        vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_setLayout),
        "Failed to create G-buffer descriptor set layout");

    // Zero bindings: the set 1 placeholder. Nothing binds it and no shader reads it.
    VkDescriptorSetLayoutCreateInfo emptyInfo{};
    emptyInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    CheckVulkan(
        vkCreateDescriptorSetLayout(m_device, &emptyInfo, nullptr, &m_emptySetLayout),
        "Failed to create empty descriptor set layout");
}

void VulkanGBufferDescriptors::CreateSampler()
{
    // Every consumer samples one texel per pixel at matching resolution. Linear filtering would
    // average a surface's normal and depth with its neighbor's across every silhouette.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    samplerInfo.mipLodBias = 0.0f;

    CheckVulkan(vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler), "Failed to create G-buffer sampler");
}

void VulkanGBufferDescriptors::CreateDescriptorSets(const SceneRenderTargets& targets)
{
    // Every input is transient, so one set per frame slot, and the slot indexes both the set and
    // each view written into it.
    const uint32_t copyCount = targets.GetTransientCopyCount();

    if (m_descriptorPool == VK_NULL_HANDLE)
    {
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = copyCount * static_cast<uint32_t>(kInputs.size());

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = copyCount;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;

        CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "Failed to create G-buffer descriptor pool");
    }
    else
    {
        // Called again from OnTargetsRebuilt because the views changed. Resetting the pool returns
        // the previous sets to it instead of leaking them.
        m_descriptorSets.clear();
        CheckVulkan(vkResetDescriptorPool(m_device, m_descriptorPool, 0), "Failed to reset G-buffer descriptor pool");
    }

    const std::vector<VkDescriptorSetLayout> layouts(copyCount, m_setLayout);

    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = m_descriptorPool;
    allocateInfo.descriptorSetCount = copyCount;
    allocateInfo.pSetLayouts = layouts.data();

    m_descriptorSets.assign(copyCount, VK_NULL_HANDLE);
    CheckVulkan(vkAllocateDescriptorSets(m_device, &allocateInfo, m_descriptorSets.data()), "Failed to allocate G-buffer descriptor sets");

    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        std::array<VkDescriptorImageInfo, kInputs.size()> imageInfos{};
        std::array<VkWriteDescriptorSet, kInputs.size()> writes{};

        for (uint32_t binding = 0; binding < static_cast<uint32_t>(kInputs.size()); ++binding)
        {
            imageInfos[binding].sampler = m_sampler;
            imageInfos[binding].imageView = targets.GetSampledView(kInputs[binding], slot);
            imageInfos[binding].imageLayout = kReadLayout;

            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = m_descriptorSets[slot];
            writes[binding].dstBinding = binding;
            writes[binding].dstArrayElement = 0;
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[binding].descriptorCount = 1;
            writes[binding].pImageInfo = &imageInfos[binding];
        }

        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void VulkanGBufferDescriptors::DestroyHandles()
{
    // Destroying the pool frees every set allocated from it.
    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    m_descriptorSets.clear();
    if (m_sampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    if (m_emptySetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_emptySetLayout, nullptr);
        m_emptySetLayout = VK_NULL_HANDLE;
    }
    if (m_setLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
        m_setLayout = VK_NULL_HANDLE;
    }
}
}
