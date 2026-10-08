#pragma once

// Deliberately not including "common.h"; see render_target_layout.h for why.
#include <nvrhi/nvrhi.h>
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
