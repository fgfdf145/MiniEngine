#include "imgui_layer.h"

#include "../imgui/imgui_impl_sdl3.h"
#include "../imgui/imgui_impl_vulkan.h"

#include <engine/core/paths/engine_paths.h>

#include <imgui.h>
#include <array>
#include <filesystem>

namespace me
{

namespace
{
constexpr uint32_t kImGuiDescriptorCount = 128;
constexpr float kDefaultUiFontSizePixels = 16.0f;

std::string BuildImGuiIniPath()
{
    return (EnginePaths::ProjectRoot() / "imgui.ini").string();
}

std::filesystem::path FindPreferredUiFontPath()
{
#if defined(_WIN32)
    constexpr std::array<const char*, 4> kCandidates = {
        "C:/Windows/Fonts/segoeuivariable.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/tahoma.ttf"};
#else
    constexpr std::array<const char*, 4> kCandidates = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/opentype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"};
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

    // Colors mirror the Claude desktop app's dark theme design tokens (--cds-*).
    auto rgb = [](int r, int g, int b, float a = 1.0f)
    {
        return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, a);
    };
    auto white = [](float a)
    {
        return ImVec4(1.0f, 1.0f, 1.0f, a);
    };

    const ImVec4 surface0 = rgb(11, 11, 11);          // --cds-surface-0
    const ImVec4 surface1 = rgb(21, 21, 21);          // --cds-surface-1
    const ImVec4 surface2 = rgb(26, 26, 25);          // --cds-surface-2 / panel
    const ImVec4 surface3 = rgb(32, 32, 31);          // --cds-surface-3 / popover
    const ImVec4 textPrimary = rgb(240, 239, 236);    // --cds-text-primary
    const ImVec4 textSecondary = rgb(195, 194, 183);  // --cds-text-secondary
    const ImVec4 textMuted = rgb(137, 135, 129);      // --cds-text-muted
    const ImVec4 textAccent = rgb(217, 119, 87);      // --cds-fill-brand-hover
    const ImVec4 fillAccent = rgb(198, 97, 63);       // --cds-fill-brand
    const ImVec4 fillAccentHover = rgb(217, 119, 87); // --cds-fill-brand-hover
    const ImVec4 fillBrandHover = rgb(217, 119, 87);  // --cds-fill-brand-hover
    const ImVec4 fillWarning = rgb(250, 178, 25);     // --cds-fill-warning
    const ImVec4 fillWarningHover = rgb(237, 161, 0); // --cds-fill-warning-hover
    const ImVec4 clear = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = textPrimary;
    colors[ImGuiCol_TextDisabled] = textMuted;
    colors[ImGuiCol_WindowBg] = surface1;
    colors[ImGuiCol_ChildBg] = clear;
    colors[ImGuiCol_PopupBg] = surface3;
    colors[ImGuiCol_Border] = white(0.10f); // --cds-border
    colors[ImGuiCol_BorderShadow] = clear;
    colors[ImGuiCol_FrameBg] = white(0.05f);         // --cds-fill-field
    colors[ImGuiCol_FrameBgHovered] = white(0.075f); // --cds-fill-ghost-hover
    colors[ImGuiCol_FrameBgActive] = white(0.10f);   // --cds-fill-control
    colors[ImGuiCol_TitleBg] = surface0;
    colors[ImGuiCol_TitleBgActive] = surface0;
    colors[ImGuiCol_TitleBgCollapsed] = surface0;
    colors[ImGuiCol_MenuBarBg] = surface0;
    colors[ImGuiCol_ScrollbarBg] = clear;
    colors[ImGuiCol_ScrollbarGrab] = white(0.20f);        // --cds-alpha-3
    colors[ImGuiCol_ScrollbarGrabHovered] = white(0.35f); // --cds-alpha-4
    colors[ImGuiCol_ScrollbarGrabActive] = white(0.50f);  // --cds-alpha-5
    colors[ImGuiCol_CheckMark] = fillAccent;
    colors[ImGuiCol_SliderGrab] = fillAccent;
    colors[ImGuiCol_SliderGrabActive] = fillAccentHover;
    colors[ImGuiCol_Button] = white(0.10f);         // --cds-fill-secondary
    colors[ImGuiCol_ButtonHovered] = white(0.14f);  // --cds-fill-secondary-hover
    colors[ImGuiCol_ButtonActive] = white(0.20f);   // --cds-fill-control-hover
    colors[ImGuiCol_Header] = white(0.10f);         // --cds-bg-neutral-hover
    colors[ImGuiCol_HeaderHovered] = white(0.075f); // --cds-fill-ghost-hover
    colors[ImGuiCol_HeaderActive] = white(0.15f);   // --cds-fill-ghost-selected
    colors[ImGuiCol_Separator] = white(0.10f);
    colors[ImGuiCol_SeparatorHovered] = white(0.40f); // --cds-border-stronger
    colors[ImGuiCol_SeparatorActive] = fillAccent;
    colors[ImGuiCol_ResizeGrip] = white(0.10f);
    colors[ImGuiCol_ResizeGripHovered] = white(0.20f);
    colors[ImGuiCol_ResizeGripActive] = fillAccent;
    colors[ImGuiCol_InputTextCursor] = textPrimary;
    colors[ImGuiCol_TabHovered] = white(0.075f);
    colors[ImGuiCol_Tab] = surface0;
    colors[ImGuiCol_TabSelected] = surface1;
    colors[ImGuiCol_TabSelectedOverline] = fillAccent;
    colors[ImGuiCol_TabDimmed] = surface0;
    colors[ImGuiCol_TabDimmedSelected] = surface2;
    colors[ImGuiCol_TabDimmedSelectedOverline] = clear;
    colors[ImGuiCol_DockingPreview] = ImVec4(fillAccent.x, fillAccent.y, fillAccent.z, 0.35f);
    colors[ImGuiCol_DockingEmptyBg] = surface0;
    colors[ImGuiCol_PlotLines] = textSecondary;
    colors[ImGuiCol_PlotLinesHovered] = fillBrandHover;
    colors[ImGuiCol_PlotHistogram] = fillWarning;
    colors[ImGuiCol_PlotHistogramHovered] = fillWarningHover;
    colors[ImGuiCol_TableHeaderBg] = surface3;
    colors[ImGuiCol_TableBorderStrong] = white(0.20f); // --cds-border-strong
    colors[ImGuiCol_TableBorderLight] = white(0.10f);
    colors[ImGuiCol_TableRowBg] = clear;
    colors[ImGuiCol_TableRowBgAlt] = white(0.05f); // --cds-alpha-1
    colors[ImGuiCol_TextLink] = textAccent;
    colors[ImGuiCol_TextSelectedBg] = ImVec4(fillAccent.x, fillAccent.y, fillAccent.z, 0.35f);
    colors[ImGuiCol_TreeLines] = white(0.20f);
    colors[ImGuiCol_DragDropTarget] = fillAccent;
    colors[ImGuiCol_DragDropTargetBg] = clear;
    colors[ImGuiCol_UnsavedMarker] = textPrimary;
    colors[ImGuiCol_NavCursor] = fillAccent;
    colors[ImGuiCol_NavWindowingHighlight] = white(0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.50f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.50f);
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
            fontConfig.GlyphRanges);
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
    VkQueue graphicsQueue)
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
}
