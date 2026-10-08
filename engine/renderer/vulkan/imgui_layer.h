#pragma once

#include "common.h"

#include <SDL3/SDL.h>

#include <string>

// ImGui's own type, declared in the global namespace like the rest of ImGui.
struct ImDrawData;

namespace me
{

class VulkanImGuiLayer
{
  public:
    VulkanImGuiLayer(
        SDL_Window* window,
        VkInstance instance,
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        uint32_t graphicsQueueFamily,
        VkQueue graphicsQueue);
    ~VulkanImGuiLayer();

    VulkanImGuiLayer(const VulkanImGuiLayer&) = delete;
    VulkanImGuiLayer& operator=(const VulkanImGuiLayer&) = delete;

    // The main thread's side: events and the frame's UI.
    void ProcessEvent(const SDL_Event& event);
    void BeginFrame();
    ImDrawData* GetDrawData() const;
    bool WantsKeyboardCapture() const;
    bool WantsMouseCapture() const;

    // The render side, used while the main thread builds no UI (construction, swapchain rebuilds).
    // renderPass is the SDR swapchain's, or with HDR output the SDR UI layer's (VulkanHdrComposite):
    // ImGui always draws as it does into an SDR swapchain.
    void CreateOrUpdateVulkanResources(VkRenderPass renderPass, uint32_t imageCount);
    void DestroyVulkanResources();

  private:
    void CreateDescriptorPool();
    void RestorePlatformBackend();
    static void CheckVkResult(VkResult result);

    SDL_Window* m_window = nullptr;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    uint32_t m_graphicsQueueFamily = 0;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    bool m_vulkanBackendInitialized = false;
    std::string m_iniFilePath;
};
}
