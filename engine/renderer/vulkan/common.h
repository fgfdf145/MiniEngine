#pragma once

#include <SDL3/SDL.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#endif
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

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

struct alignas(16) MaterialPushConstants
{
    float baseColorFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float emissiveFactor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float surfaceFactors[4] = { 0.0f, 1.0f, 1.0f, 1.0f };
};

struct alignas(16) ObjectPushConstants
{
    glm::mat4 model{ 1.0f };
    MaterialPushConstants material;
};

inline void CheckVulkan(VkResult result, const char* message)
{
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error(std::string(message) + " (VkResult=" + std::to_string(static_cast<int>(result)) + ")");
    }
}
