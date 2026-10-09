#pragma once

// Deliberately not including "common.h"; see render_target_layout.h for why.
#include <nvrhi/nvrhi.h>
#include <nvrhi/vulkan.h>
#include <vulkan/vulkan.h>

#include <stdexcept>
#include <type_traits>

namespace me
{

// What an NVRHI object's getNativeObject returns, as the Vulkan handle type the code that still
// records Vulkan directly takes. Non-dispatchable handles are pointers on 64-bit targets and 64-bit
// integers on 32-bit ones; nvrhi::Object holds either.
template <typename VkHandle>
VkHandle ToNative(nvrhi::Object object)
{
    if constexpr (std::is_pointer_v<VkHandle>)
    {
        return static_cast<VkHandle>(object.pointer);
    }
    else
    {
        return static_cast<VkHandle>(object.integer);
    }
}

// The NVRHI format whose Vulkan format is format, or UNKNOWN. Where two share one (D24S8 and
// X24G8_UINT), the depth format, which comes first.
// A native image view, made only on Vulkan: on Direct3D 12 there is no VkDevice (null), nothing
// reads the view, and the call makes none and succeeds.
inline VkResult CreateNativeImageView(VkDevice device, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* allocator, VkImageView* view)
{
    if (device == VK_NULL_HANDLE)
    {
        *view = VK_NULL_HANDLE;
        return VK_SUCCESS;
    }
    return vkCreateImageView(device, info, allocator, view);
}

inline void DestroyNativeImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks* allocator)
{
    if (device != VK_NULL_HANDLE && view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device, view, allocator);
    }
}

inline nvrhi::Format ToNvrhiFormat(VkFormat format)
{
    for (uint32_t index = 1; index < static_cast<uint32_t>(nvrhi::Format::COUNT); ++index)
    {
        const nvrhi::Format candidate = static_cast<nvrhi::Format>(index);
        if (nvrhi::vulkan::convertFormat(candidate) == format)
        {
            return candidate;
        }
    }
    return nvrhi::Format::UNKNOWN;
}

inline VkSampler NativeSampler(nvrhi::ISampler* sampler)
{
    return sampler != nullptr ? ToNative<VkSampler>(sampler->getNativeObject(nvrhi::ObjectTypes::VK_Sampler)) : VK_NULL_HANDLE;
}

// Throws failureMessage when the device cannot make the sampler.
inline nvrhi::SamplerHandle CreateNvrhiSampler(nvrhi::IDevice* device, const nvrhi::SamplerDesc& desc, const char* failureMessage)
{
    nvrhi::SamplerHandle sampler = device->createSampler(desc);
    if (!sampler)
    {
        throw std::runtime_error(failureMessage);
    }
    return sampler;
}
}
