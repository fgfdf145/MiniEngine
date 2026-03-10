#include "renderer.h"

#include <log/log.h>
#include <window/window.h>

namespace
{
void LogVulkanRuntimeInfo()
{
    uint32_t apiVersion = 0;
    CheckVulkan(vkEnumerateInstanceVersion(&apiVersion), "Failed to query Vulkan runtime version");
    LOG_INFO(
        "Vulkan runtime API version: {}.{}.{}",
        VK_API_VERSION_MAJOR(apiVersion),
        VK_API_VERSION_MINOR(apiVersion),
        VK_API_VERSION_PATCH(apiVersion)
    );
}
}

VulkanRenderer::VulkanRenderer(Window& window)
    : m_window(window)
{
    LOG_INFO("VULKAN_SDK: {}", "C:/VulkanSDK/1.4.341.1");
    LogVulkanRuntimeInfo();
    LoadVulkanLoader();

    m_instance = std::make_unique<VulkanInstance>(m_window.GetSDLWindow());
    m_device = std::make_unique<VulkanDevice>(m_instance->GetHandle(), m_instance->GetSurface());
    m_vertexBuffer = std::make_unique<VulkanBuffer>(m_device->GetPhysicalDevice(), m_device->GetHandle());
    CreateSwapchainResources();
}

VulkanRenderer::~VulkanRenderer()
{
    if (m_device)
    {
        vkDeviceWaitIdle(m_device->GetHandle());
    }

    DestroySwapchainResources();
    m_vertexBuffer.reset();
    m_device.reset();
    m_instance.reset();
    UnloadVulkanLoader();
}

void VulkanRenderer::DrawFrame()
{
    if (!HasDrawableArea())
    {
        return;
    }

    uint32_t imageIndex = 0;
    const VkResult acquireResult = m_commandContext->AcquireNextImage(m_swapchain->GetHandle(), imageIndex);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        RecreateSwapchain();
        return;
    }

    CheckVulkan(acquireResult, "Failed to acquire swapchain image");
    m_commandContext->Submit(m_device->GetGraphicsQueue(), imageIndex);

    const VkResult presentResult = m_commandContext->Present(m_device->GetPresentQueue(), m_swapchain->GetHandle(), imageIndex);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
    {
        RecreateSwapchain();
        return;
    }

    CheckVulkan(presentResult, "Failed to present swapchain image");
}

void VulkanRenderer::LoadVulkanLoader()
{
    if (SDL_Vulkan_LoadLibrary("C:/VulkanSDK/1.4.341.1/Bin/vulkan-1.dll"))
    {
        m_vulkanLoaderLoaded = true;
        LOG_INFO("Using Vulkan SDK loader: {}", "C:/VulkanSDK/1.4.341.1/Bin/vulkan-1.dll");
        return;
    }

    if (!SDL_Vulkan_LoadLibrary(nullptr))
    {
        throw std::runtime_error("Failed to load a Vulkan loader through both SDK path and SDL default search path");
    }

    m_vulkanLoaderLoaded = true;
    LOG_INFO("Using default Vulkan loader");
}

void VulkanRenderer::UnloadVulkanLoader()
{
    if (m_vulkanLoaderLoaded)
    {
        SDL_Vulkan_UnloadLibrary();
        m_vulkanLoaderLoaded = false;
    }
}

void VulkanRenderer::CreateSwapchainResources()
{
    const SwapchainSupportDetails supportDetails = m_device->QuerySwapchainSupport();
    m_swapchain = std::make_unique<VulkanSwapchain>(
        m_window.GetSDLWindow(),
        m_device->GetHandle(),
        m_instance->GetSurface(),
        m_device->GetQueueFamilies(),
        supportDetails
    );
    m_renderPass = std::make_unique<VulkanRenderPass>(
        m_device->GetHandle(),
        m_swapchain->GetImageFormat(),
        m_swapchain->GetExtent(),
        m_swapchain->GetImageViews()
    );
    m_graphicsPipeline = std::make_unique<VulkanPipeline>(
        m_device->GetHandle(),
        m_swapchain->GetExtent(),
        m_renderPass->GetHandle()
    );
    m_commandContext = std::make_unique<VulkanCommandContext>(
        m_device->GetHandle(),
        m_device->GetQueueFamilies(),
        m_renderPass->GetHandle(),
        m_graphicsPipeline->GetHandle(),
        m_swapchain->GetExtent(),
        m_vertexBuffer->GetHandle(),
        m_vertexBuffer->GetVertexCount(),
        m_renderPass->GetFramebuffers()
    );
}

void VulkanRenderer::DestroySwapchainResources()
{
    m_commandContext.reset();
    m_graphicsPipeline.reset();
    m_renderPass.reset();
    m_swapchain.reset();
}

void VulkanRenderer::RecreateSwapchain()
{
    if (!HasDrawableArea())
    {
        return;
    }

    vkDeviceWaitIdle(m_device->GetHandle());
    DestroySwapchainResources();
    CreateSwapchainResources();
}

bool VulkanRenderer::HasDrawableArea() const
{
    int width = 0;
    int height = 0;
    if (!SDL_GetWindowSizeInPixels(m_window.GetSDLWindow(), &width, &height))
    {
        throw std::runtime_error(std::string("SDL_GetWindowSizeInPixels failed: ") + SDL_GetError());
    }

    return width > 0 && height > 0;
}
