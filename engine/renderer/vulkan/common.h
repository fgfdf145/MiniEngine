#pragma once

#include <SDL3/SDL.h>
#include "../material.h"
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace me
{

struct QueueFamilyIndices
{
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    bool IsComplete() const
    {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

struct SwapchainSupportDetails
{
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

// What CheckVulkan throws. Carrying the result lets a caller that can recover from one kind of
// failure (running out of memory during a content upload) catch exactly that and let every other
// failure, such as a lost device, keep propagating.
class VulkanError : public std::runtime_error
{
  public:
    VulkanError(VkResult result, const std::string& message)
        : std::runtime_error(message),
          m_result(result)
    {
    }

    VkResult GetResult() const
    {
        return m_result;
    }

    // Device or host memory, or the driver's allocation count limit, ran out. Freeing resources
    // can make the same request succeed later, which is what makes this failure recoverable.
    bool IsOutOfMemory() const
    {
        return m_result == VK_ERROR_OUT_OF_DEVICE_MEMORY ||
               m_result == VK_ERROR_OUT_OF_HOST_MEMORY ||
               m_result == VK_ERROR_TOO_MANY_OBJECTS;
    }

  private:
    VkResult m_result = VK_SUCCESS;
};

inline void CheckVulkan(VkResult result, const char* message)
{
    if (result != VK_SUCCESS)
    {
        throw VulkanError(result, std::string(message) + " (VkResult=" + std::to_string(static_cast<int>(result)) + ")");
    }
}
}
