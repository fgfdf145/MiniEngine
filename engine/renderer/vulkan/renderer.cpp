#include "renderer.h"

#include "../imgui/imgui_impl_vulkan.h"

#include <imgui.h>
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

void UpdateCameraFromInput(Camera& camera, const InputState& input, float deltaTime, bool blockKeyboardInput)
{
    const float moveDistance = camera.moveSpeed * deltaTime;

    if (!blockKeyboardInput)
    {
        if (input.IsKeyDown(SDL_SCANCODE_W))
        {
            camera.MoveForward(moveDistance);
        }
        if (input.IsKeyDown(SDL_SCANCODE_S))
        {
            camera.MoveForward(-moveDistance);
        }
        if (input.IsKeyDown(SDL_SCANCODE_A))
        {
            camera.MoveRight(-moveDistance);
        }
        if (input.IsKeyDown(SDL_SCANCODE_D))
        {
            camera.MoveRight(moveDistance);
        }
    }

    if (input.IsMouseLookActive())
    {
        camera.Rotate(
            input.GetMouseDeltaX() * camera.mouseSensitivity,
            -input.GetMouseDeltaY() * camera.mouseSensitivity
        );
    }
}
}

VulkanRenderer::VulkanRenderer(Window& window)
    : m_window(window)
{
    LogVulkanRuntimeInfo();

    m_instance = std::make_unique<VulkanInstance>(m_window.GetSDLWindow());
    m_device = std::make_unique<VulkanDevice>(m_instance->GetHandle(), m_instance->GetSurface());
    m_vertexBuffer = std::make_unique<VulkanBuffer>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_device->GetQueueFamilies().graphicsFamily.value(),
        m_device->GetGraphicsQueue()
    );
    m_imguiLayer = std::make_unique<VulkanImGuiLayer>(
        m_window.GetSDLWindow(),
        m_instance->GetHandle(),
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_device->GetQueueFamilies().graphicsFamily.value(),
        m_device->GetGraphicsQueue()
    );
    CreateSwapchainResources();
}

VulkanRenderer::~VulkanRenderer()
{
    if (m_device)
    {
        vkDeviceWaitIdle(m_device->GetHandle());
    }

    DestroySwapchainResources();
    m_imguiLayer.reset();
    m_vertexBuffer.reset();
    m_device.reset();
    m_instance.reset();
}

void VulkanRenderer::HandleEvent(const SDL_Event& event)
{
    m_input.HandleEvent(event);
    m_imguiLayer->ProcessEvent(event);

    if ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
        event.button.button == SDL_BUTTON_RIGHT)
    {
        SDL_SetWindowRelativeMouseMode(m_window.GetSDLWindow(), event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
    }
}

void VulkanRenderer::DrawFrame()
{
    const auto currentFrameTime = std::chrono::steady_clock::now();
    const float deltaTime = std::chrono::duration<float>(currentFrameTime - m_lastFrameTime).count();
    m_lastFrameTime = currentFrameTime;

    UpdateCameraFromInput(m_camera, m_input, deltaTime, m_imguiLayer->WantsKeyboardCapture());
    m_input.EndFrame();

    if (!HasDrawableArea())
    {
        return;
    }

    m_imguiLayer->BeginFrame();
    m_imguiLayer->DrawCameraControls(m_camera);
    ImGui::Render();

    uint32_t imageIndex = 0;
    const VkResult acquireResult = m_commandContext->AcquireNextImage(m_swapchain->GetHandle(), imageIndex);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        RecreateSwapchain();
        return;
    }

    CheckVulkan(acquireResult, "Failed to acquire swapchain image");
    m_uniformBuffer->Update(imageIndex, m_camera, m_swapchain->GetExtent());
    m_commandContext->RecordCommandBuffer(
        imageIndex,
        m_renderPass->GetHandle(),
        m_renderPass->GetFramebuffers()[imageIndex],
        m_swapchain->GetExtent(),
        m_graphicsPipeline->GetHandle(),
        m_graphicsPipeline->GetLayout(),
        m_vertexBuffer->GetVertexHandle(),
        m_vertexBuffer->GetIndexHandle(),
        m_vertexBuffer->GetIndexCount(),
        m_uniformBuffer->GetDescriptorSet(imageIndex),
        [](VkCommandBuffer commandBuffer)
        {
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
        }
    );
    m_commandContext->Submit(m_device->GetGraphicsQueue(), imageIndex);

    const VkResult presentResult = m_commandContext->Present(m_device->GetPresentQueue(), m_swapchain->GetHandle(), imageIndex);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
    {
        RecreateSwapchain();
        return;
    }

    CheckVulkan(presentResult, "Failed to present swapchain image");
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
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        m_swapchain->GetImageFormat(),
        m_swapchain->GetExtent(),
        m_swapchain->GetImageViews()
    );
    m_uniformBuffer = std::make_unique<VulkanUniformBuffer>(
        m_device->GetPhysicalDevice(),
        m_device->GetHandle(),
        static_cast<uint32_t>(m_swapchain->GetImageViews().size())
    );
    m_graphicsPipeline = std::make_unique<VulkanPipeline>(
        m_device->GetHandle(),
        m_swapchain->GetExtent(),
        m_renderPass->GetHandle(),
        m_uniformBuffer->GetDescriptorSetLayout()
    );
    m_commandContext = std::make_unique<VulkanCommandContext>(m_device->GetHandle(), m_device->GetQueueFamilies(), m_renderPass->GetFramebuffers().size());
    m_imguiLayer->CreateOrUpdateVulkanResources(m_renderPass->GetHandle(), static_cast<uint32_t>(m_swapchain->GetImageViews().size()));
}

void VulkanRenderer::DestroySwapchainResources()
{
    if (m_imguiLayer)
    {
        m_imguiLayer->DestroyVulkanResources();
    }
    m_commandContext.reset();
    m_graphicsPipeline.reset();
    m_uniformBuffer.reset();
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
