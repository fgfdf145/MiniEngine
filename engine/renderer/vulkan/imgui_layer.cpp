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
    style.WindowRounding = 4.0f;
    style.ChildRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;
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
    const ImVec4 baseBg = ImVec4(0.141f, 0.141f, 0.141f, 1.0f);          // #242424
    const ImVec4 elevatedBg = ImVec4(0.165f, 0.165f, 0.165f, 1.0f);      // #2a2a2a
    const ImVec4 activeBg = ImVec4(0.188f, 0.188f, 0.188f, 1.0f);        // #303030
    const ImVec4 hoverBg = ImVec4(0.212f, 0.212f, 0.212f, 1.0f);         // #363636
    const ImVec4 strongBg = ImVec4(0.251f, 0.251f, 0.251f, 1.0f);        // #404040
    const ImVec4 border = ImVec4(0.314f, 0.314f, 0.314f, 1.0f);          // #505050
    const ImVec4 accent = ImVec4(0.380f, 0.380f, 0.380f, 1.0f);          // #616161
    colors[ImGuiCol_Text] = ImVec4(0.86f, 0.86f, 0.86f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.56f, 0.56f, 0.56f, 1.0f);
    colors[ImGuiCol_WindowBg] = baseBg;
    colors[ImGuiCol_ChildBg] = baseBg;
    colors[ImGuiCol_PopupBg] = ImVec4(0.141f, 0.141f, 0.141f, 0.98f);
    colors[ImGuiCol_Border] = border;
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_FrameBg] = elevatedBg;
    colors[ImGuiCol_FrameBgHovered] = hoverBg;
    colors[ImGuiCol_FrameBgActive] = strongBg;
    colors[ImGuiCol_TitleBg] = baseBg;
    colors[ImGuiCol_TitleBgActive] = elevatedBg;
    colors[ImGuiCol_TitleBgCollapsed] = baseBg;
    colors[ImGuiCol_MenuBarBg] = elevatedBg;
    colors[ImGuiCol_ScrollbarBg] = baseBg;
    colors[ImGuiCol_ScrollbarGrab] = activeBg;
    colors[ImGuiCol_ScrollbarGrabHovered] = hoverBg;
    colors[ImGuiCol_ScrollbarGrabActive] = strongBg;
    colors[ImGuiCol_CheckMark] = accent;
    colors[ImGuiCol_SliderGrab] = accent;
    colors[ImGuiCol_SliderGrabActive] = strongBg;
    colors[ImGuiCol_Button] = elevatedBg;
    colors[ImGuiCol_ButtonHovered] = hoverBg;
    colors[ImGuiCol_ButtonActive] = strongBg;
    colors[ImGuiCol_Header] = elevatedBg;
    colors[ImGuiCol_HeaderHovered] = hoverBg;
    colors[ImGuiCol_HeaderActive] = strongBg;
    colors[ImGuiCol_Separator] = border;
    colors[ImGuiCol_SeparatorHovered] = hoverBg;
    colors[ImGuiCol_SeparatorActive] = strongBg;
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.380f, 0.380f, 0.380f, 0.25f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.380f, 0.380f, 0.380f, 0.55f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.380f, 0.380f, 0.380f, 0.90f);
    colors[ImGuiCol_Tab] = baseBg;
    colors[ImGuiCol_TabHovered] = hoverBg;
    colors[ImGuiCol_TabActive] = elevatedBg;
    colors[ImGuiCol_TabUnfocused] = baseBg;
    colors[ImGuiCol_TabUnfocusedActive] = activeBg;
    colors[ImGuiCol_DockingPreview] = ImVec4(0.380f, 0.380f, 0.380f, 0.24f);
    colors[ImGuiCol_DockingEmptyBg] = baseBg;
    colors[ImGuiCol_TableHeaderBg] = elevatedBg;
    colors[ImGuiCol_TableBorderStrong] = border;
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.220f, 0.220f, 0.220f, 1.0f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.380f, 0.380f, 0.380f, 0.35f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(0.380f, 0.380f, 0.380f, 0.90f);
    colors[ImGuiCol_NavCursor] = ImVec4(0.380f, 0.380f, 0.380f, 1.0f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.380f, 0.380f, 0.380f, 0.70f);
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
