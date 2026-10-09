#include "gbuffer_inputs.h"

#include "nvrhi_pass.h"
#include "sampler_settings.h"

#include <stdexcept>

namespace me
{

VulkanGBufferDescriptors::VulkanGBufferDescriptors(VkDevice device, nvrhi::IDevice* nvrhiDevice, const SceneRenderTargets& targets)
    : m_device(device),
      m_nvrhiDevice(nvrhiDevice)
{
    try
    {
        CreateSetLayouts();
        CreateSampler(nvrhiDevice);
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
    return ToNative<VkDescriptorSetLayout>(m_setLayout->getNativeObject(nvrhi::ObjectTypes::VK_DescriptorSetLayout));
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

nvrhi::IBindingLayout* VulkanGBufferDescriptors::GetBindingLayout() const
{
    return m_setLayout;
}

nvrhi::IBindingSet* VulkanGBufferDescriptors::GetBindingSet(
    const SceneRenderTargets& targets,
    uint32_t imageIndex,
    uint32_t frameSlot) const
{
    return m_bindingSets.at(targets.ResolveIndex(kInputs.front(), imageIndex, frameSlot));
}

void VulkanGBufferDescriptors::OnTargetsRebuilt(const SceneRenderTargets& targets)
{
    CreateDescriptorSets(targets);
}

void VulkanGBufferDescriptors::CreateSetLayouts()
{
    nvrhi::BindingLayoutDesc desc;
    desc.visibility = nvrhi::ShaderType::Pixel;
    desc.registerSpace = 2;
    desc.registerSpaceIsDescriptorSet = true;
    desc.bindingOffsets = ShaderBindingOffsets();
    for (uint32_t binding = 0; binding < static_cast<uint32_t>(kInputs.size()); ++binding)
    {
        desc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(binding));
    }
    desc.bindings.push_back(nvrhi::BindingLayoutItem::Sampler(kSamplerBinding));
    m_setLayout = m_nvrhiDevice->createBindingLayout(desc);
    if (!m_setLayout)
    {
        throw std::runtime_error("Failed to create G-buffer descriptor set layout");
    }

    // Zero bindings: the set 1 placeholder. Nothing binds it and no shader reads it.
    VkDescriptorSetLayoutCreateInfo emptyInfo{};
    emptyInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    CheckVulkan(
        vkCreateDescriptorSetLayout(m_device, &emptyInfo, nullptr, &m_emptySetLayout),
        "Failed to create empty descriptor set layout");
}

void VulkanGBufferDescriptors::CreateSampler(nvrhi::IDevice* nvrhiDevice)
{
    // Every consumer samples one texel per pixel at matching resolution. Linear filtering would
    // average a surface's normal and depth with its neighbor's across every silhouette.
    m_sampler = CreateNvrhiSampler(nvrhiDevice, BuildClampSamplerDesc(false), "Failed to create G-buffer sampler");
}

void VulkanGBufferDescriptors::CreateDescriptorSets(const SceneRenderTargets& targets)
{
    // Every input is transient, so one set per frame slot, and the slot indexes both the set and
    // each copy bound in it. Called again from OnTargetsRebuilt because the targets changed: NVRHI's
    // sets are made anew (the old ones release their textures as they go).
    const uint32_t copyCount = targets.GetTransientCopyCount();
    std::vector<nvrhi::BindingSetHandle> sets(copyCount);
    std::vector<VkDescriptorSet> nativeSets(copyCount);
    for (uint32_t slot = 0; slot < copyCount; ++slot)
    {
        nvrhi::BindingSetDesc desc;
        for (uint32_t binding = 0; binding < static_cast<uint32_t>(kInputs.size()); ++binding)
        {
            // The whole texture, depth alone for a depth and stencil target (NVRHI's view of it
            // is GetSampledView's).
            desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(binding, targets.GetTexture(kInputs[binding], slot)));
        }
        desc.bindings.push_back(nvrhi::BindingSetItem::Sampler(kSamplerBinding, m_sampler));
        sets[slot] = m_nvrhiDevice->createBindingSet(desc, m_setLayout);
        if (!sets[slot])
        {
            throw std::runtime_error("Failed to create a G-buffer binding set");
        }
        nativeSets[slot] = ToNative<VkDescriptorSet>(sets[slot]->getNativeObject(nvrhi::ObjectTypes::VK_DescriptorSet));
    }
    m_bindingSets = std::move(sets);
    m_descriptorSets = std::move(nativeSets);
}

void VulkanGBufferDescriptors::DestroyHandles()
{
    m_descriptorSets.clear();
    m_bindingSets.clear();
    m_sampler = nullptr;
    if (m_emptySetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(m_device, m_emptySetLayout, nullptr);
        m_emptySetLayout = VK_NULL_HANDLE;
    }
    m_setLayout = nullptr;
}
}
