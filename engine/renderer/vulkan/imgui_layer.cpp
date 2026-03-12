#include "imgui_layer.h"

#include "../imgui/imgui_impl_sdl3.h"
#include "../imgui/imgui_impl_vulkan.h"

#include <file_dialog/file_dialog.h>
#include <imgui.h>

namespace
{
constexpr uint32_t kImGuiDescriptorCount = 128;
}

VulkanImGuiLayer::VulkanImGuiLayer(
    SDL_Window* window,
    VkInstance instance,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    uint32_t graphicsQueueFamily,
    VkQueue graphicsQueue
)
    : m_window(window),
      m_instance(instance),
      m_physicalDevice(physicalDevice),
      m_device(device),
      m_graphicsQueueFamily(graphicsQueueFamily),
      m_graphicsQueue(graphicsQueue)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    CreateDescriptorPool();

    if (!ImGui_ImplSDL3_InitForVulkan(m_window))
    {
        throw std::runtime_error("Failed to initialize ImGui SDL3 backend");
    }
}

VulkanImGuiLayer::~VulkanImGuiLayer()
{
    DestroyVulkanResources();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    if (m_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    }
}

void VulkanImGuiLayer::ProcessEvent(const SDL_Event& event)
{
    ImGui_ImplSDL3_ProcessEvent(&event);
}

void VulkanImGuiLayer::BeginFrame()
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void VulkanImGuiLayer::DrawCameraControls(Camera& camera)
{
    ImGui::Begin("Camera");
    ImGui::Text("Move: WASD");
    ImGui::Text("Look: Hold Right Mouse");
    ImGui::SliderFloat3("Position", &camera.position.x, -10.0f, 10.0f);
    ImGui::SliderFloat("Yaw", &camera.yawDegrees, -180.0f, 180.0f);
    ImGui::SliderFloat("Pitch", &camera.pitchDegrees, -89.0f, 89.0f);
    ImGui::SliderFloat("Speed", &camera.moveSpeed, 0.5f, 20.0f);
    ImGui::SliderFloat("Sensitivity", &camera.mouseSensitivity, 0.01f, 1.0f);
    ImGui::SliderFloat("Fov", &camera.fovDegrees, 20.0f, 90.0f);
    ImGui::SliderFloat("Near", &camera.nearPlane, 0.01f, 5.0f);
    ImGui::SliderFloat("Far", &camera.farPlane, 1.0f, 200.0f);
    ImGui::End();
}

std::optional<std::string> VulkanImGuiLayer::DrawModelControls(const std::string& currentModelPath, const std::string& lastLoadError)
{
    std::optional<std::string> selectedPath;

    ImGui::Begin("Assets");
    if (ImGui::Button("Load Model"))
    {
        selectedPath = OpenModelFileDialog();
    }

    ImGui::Separator();
    ImGui::TextWrapped("Current Model: %s", currentModelPath.empty() ? "Default Cube" : currentModelPath.c_str());
    if (!lastLoadError.empty())
    {
        ImGui::TextWrapped("Last Error: %s", lastLoadError.c_str());
    }
    ImGui::End();

    return selectedPath;
}

ImDrawData* VulkanImGuiLayer::GetDrawData() const
{
    return ImGui::GetDrawData();
}

bool VulkanImGuiLayer::WantsKeyboardCapture() const
{
    return ImGui::GetIO().WantCaptureKeyboard;
}

bool VulkanImGuiLayer::WantsMouseCapture() const
{
    return ImGui::GetIO().WantCaptureMouse;
}

void VulkanImGuiLayer::CreateOrUpdateVulkanResources(VkRenderPass renderPass, uint32_t imageCount)
{
    DestroyVulkanResources();

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = VK_API_VERSION_1_3;
    initInfo.Instance = m_instance;
    initInfo.PhysicalDevice = m_physicalDevice;
    initInfo.Device = m_device;
    initInfo.QueueFamily = m_graphicsQueueFamily;
    initInfo.Queue = m_graphicsQueue;
    initInfo.DescriptorPool = m_descriptorPool;
    initInfo.RenderPass = renderPass;
    initInfo.MinImageCount = imageCount;
    initInfo.ImageCount = imageCount;
    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.CheckVkResultFn = &VulkanImGuiLayer::CheckVkResult;

    if (!ImGui_ImplVulkan_Init(&initInfo))
    {
        throw std::runtime_error("Failed to initialize ImGui Vulkan backend");
    }

    UploadFonts();
    m_vulkanBackendInitialized = true;
}

void VulkanImGuiLayer::DestroyVulkanResources()
{
    if (m_vulkanBackendInitialized)
    {
        vkDeviceWaitIdle(m_device);
        ImGui_ImplVulkan_Shutdown();
        m_vulkanBackendInitialized = false;
    }
}

void VulkanImGuiLayer::CreateDescriptorPool()
{
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = kImGuiDescriptorCount;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = kImGuiDescriptorCount;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;

    CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "Failed to create ImGui descriptor pool");
}

void VulkanImGuiLayer::UploadFonts() const
{
    if (!ImGui_ImplVulkan_CreateFontsTexture())
    {
        throw std::runtime_error("Failed to upload ImGui font texture");
    }
}

void VulkanImGuiLayer::CheckVkResult(VkResult result)
{
    CheckVulkan(result, "ImGui Vulkan backend call failed");
}
