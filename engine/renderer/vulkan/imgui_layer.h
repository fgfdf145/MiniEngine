#pragma once

#include "camera.h"
#include "common.h"

#include <SDL3/SDL.h>

struct ImDrawData;

class VulkanImGuiLayer
{
public:
    VulkanImGuiLayer(
        SDL_Window* window,
        VkInstance instance,
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        uint32_t graphicsQueueFamily,
        VkQueue graphicsQueue
    );
    ~VulkanImGuiLayer();

    VulkanImGuiLayer(const VulkanImGuiLayer&) = delete;
    VulkanImGuiLayer& operator=(const VulkanImGuiLayer&) = delete;

    void ProcessEvent(const SDL_Event& event);
    void BeginFrame();
    void DrawCameraControls(Camera& camera);
    ImDrawData* GetDrawData() const;
    bool WantsKeyboardCapture() const;
    bool WantsMouseCapture() const;

    void CreateOrUpdateVulkanResources(VkRenderPass renderPass, uint32_t imageCount);
    void DestroyVulkanResources();

private:
    void CreateDescriptorPool();
    void UploadFonts() const;
    static void CheckVkResult(VkResult result);

    SDL_Window* m_window = nullptr;
    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    uint32_t m_graphicsQueueFamily = 0;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    bool m_vulkanBackendInitialized = false;
};
