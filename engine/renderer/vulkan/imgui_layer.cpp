#include "imgui_layer.h"

#include "../imgui/imgui_impl_sdl3.h"
#include "../imgui/imgui_impl_vulkan.h"

#include <imgui.h>
#include <array>
#include <filesystem>

namespace
{
constexpr uint32_t kImGuiDescriptorCount = 128;
constexpr float kDefaultUiFontSizePixels = 16.0f;

std::string BuildImGuiIniPath()
{
    return (std::filesystem::path(MINIENGINE_PROJECT_DIR) / "imgui.ini").string();
}

std::filesystem::path FindPreferredUiFontPath()
{
#if defined(_WIN32)
    constexpr std::array<const char*, 4> kCandidates = {
        "C:/Windows/Fonts/segoeuivariable.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/tahoma.ttf"
    };
#elif defined(__APPLE__)
    constexpr std::array<const char*, 4> kCandidates = {
        "/System/Library/Fonts/Supplemental/SFNS.ttf",
        "/System/Library/Fonts/Supplemental/Helvetica.ttc",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Supplemental/Verdana.ttf"
    };
#else
    constexpr std::array<const char*, 4> kCandidates = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/opentype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"
    };
#endif

    for (const char* candidate : kCandidates)
    {
        std::error_code errorCode;
        if (std::filesystem::exists(candidate, errorCode) && !errorCode)
        {
            return std::filesystem::path(candidate);
        }
    }

    return {};
}

void ConfigureImGuiStyle()
{
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.CellPadding = ImVec2(8.0f, 6.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 6.0f);
    style.IndentSpacing = 22.0f;
    style.ScrollbarSize = 15.0f;
    style.GrabMinSize = 12.0f;
    style.WindowRounding = 10.0f;
    style.ChildRounding = 8.0f;
    style.PopupRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 8.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.SeparatorTextBorderSize = 1.0f;
    style.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
    style.DisplaySafeAreaPadding = ImVec2(6.0f, 6.0f);
    style.DockingSeparatorSize = 2.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.94f, 0.96f, 0.98f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.56f, 0.61f, 0.68f, 1.0f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.10f, 0.13f, 1.0f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.13f, 0.17f, 1.0f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.13f, 0.17f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.21f, 0.26f, 0.32f, 1.0f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.18f, 0.23f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.19f, 0.24f, 0.31f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.29f, 0.37f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.09f, 0.11f, 0.15f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.11f, 0.14f, 0.19f, 1.0f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.08f, 0.10f, 0.13f, 1.0f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.10f, 0.12f, 0.16f, 1.0f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.07f, 0.09f, 0.12f, 1.0f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.26f, 0.31f, 0.38f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.33f, 0.39f, 0.47f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.39f, 0.45f, 0.54f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.96f, 0.75f, 0.34f, 1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.88f, 0.68f, 0.30f, 1.0f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.98f, 0.79f, 0.40f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.18f, 0.26f, 0.36f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.35f, 0.47f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.28f, 0.40f, 0.54f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.17f, 0.24f, 0.33f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.23f, 0.33f, 0.44f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.27f, 0.39f, 0.52f, 1.0f);
    colors[ImGuiCol_Separator] = ImVec4(0.24f, 0.29f, 0.36f, 1.0f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.88f, 0.69f, 0.31f, 0.85f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.98f, 0.79f, 0.40f, 1.0f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.31f, 0.44f, 0.58f, 0.30f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.88f, 0.69f, 0.31f, 0.70f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.98f, 0.79f, 0.40f, 1.0f);
    colors[ImGuiCol_Tab] = ImVec4(0.12f, 0.15f, 0.21f, 1.0f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.23f, 0.33f, 0.44f, 1.0f);
    colors[ImGuiCol_TabActive] = ImVec4(0.18f, 0.26f, 0.36f, 1.0f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.10f, 0.13f, 0.18f, 1.0f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.14f, 0.19f, 0.26f, 1.0f);
    colors[ImGuiCol_DockingPreview] = ImVec4(0.96f, 0.75f, 0.34f, 0.24f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.07f, 0.09f, 0.12f, 1.0f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.13f, 0.17f, 0.23f, 1.0f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.24f, 0.29f, 0.36f, 1.0f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.18f, 0.22f, 0.28f, 1.0f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.28f, 0.40f, 0.54f, 0.35f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(0.98f, 0.79f, 0.40f, 0.95f);
    colors[ImGuiCol_NavCursor] = ImVec4(0.98f, 0.79f, 0.40f, 1.0f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.98f, 0.79f, 0.40f, 0.70f);
}

void ConfigureImGuiFonts(ImGuiIO& io)
{
    ImFontAtlas* fonts = io.Fonts;
    fonts->Clear();

    ImFontConfig fontConfig{};
    fontConfig.SizePixels = kDefaultUiFontSizePixels;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 1;
    fontConfig.PixelSnapH = false;
    fontConfig.RasterizerMultiply = 1.08f;
    fontConfig.GlyphRanges = fonts->GetGlyphRangesDefault();

    ImFont* defaultFont = nullptr;
    const std::filesystem::path preferredFontPath = FindPreferredUiFontPath();
    if (!preferredFontPath.empty())
    {
        const std::string preferredFontPathString = preferredFontPath.string();
        defaultFont = fonts->AddFontFromFileTTF(
            preferredFontPathString.c_str(),
            fontConfig.SizePixels,
            &fontConfig,
            fontConfig.GlyphRanges
        );
    }

    if (defaultFont == nullptr)
    {
        defaultFont = fonts->AddFontDefaultVector(&fontConfig);
    }

    io.FontDefault = defaultFont;
}

void CleanupImGuiViewportState()
{
    if (ImGui::GetCurrentContext() == nullptr)
    {
        return;
    }

    ImGui::DestroyPlatformWindows();

    if (ImGuiViewport* mainViewport = ImGui::GetMainViewport(); mainViewport != nullptr)
    {
        mainViewport->RendererUserData = nullptr;
        mainViewport->PlatformUserData = nullptr;
        mainViewport->PlatformHandle = nullptr;
        mainViewport->PlatformHandleRaw = nullptr;
    }
}
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
      m_graphicsQueue(graphicsQueue),
      m_iniFilePath(BuildImGuiIniPath())
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = m_iniFilePath.c_str();
    ConfigureImGuiStyle();
    ConfigureImGuiFonts(io);

    CreateDescriptorPool();

    if (!ImGui_ImplSDL3_InitForVulkan(m_window))
    {
        throw std::runtime_error("Failed to initialize ImGui SDL3 backend");
    }
}

VulkanImGuiLayer::~VulkanImGuiLayer()
{
    DestroyVulkanResources();
    CleanupImGuiViewportState();
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
