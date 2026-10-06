#include "imgui_layer.h"

#include "../imgui/imgui_impl_sdl3.h"
#include "../imgui/imgui_impl_vulkan.h"

#include <engine/core/paths/engine_paths.h>
#include <engine/editor/editor_icons.h>

#include <imgui.h>
#include <implot.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <span>

namespace me
{

// engine/renderer/imgui holds Dear ImGui's own Vulkan and SDL3 backends, unmodified, from the release
// vcpkg installs (vcpkg.json pins it). A backend from another release may not match imgui.h.
static_assert(IMGUI_VERSION_NUM == 19291, "Update engine/renderer/imgui to the backends of this ImGui release");

namespace
{
// Sampled images: the font atlas pages plus every ImGui_ImplVulkan_AddTexture (viewport, minimap).
constexpr uint32_t kImGuiDescriptorCount = 128;
// The Claude desktop app's body text, --cds-font-size-body (0.8125rem at its default density).
constexpr float kDefaultUiFontSizePixels = 13.0f;

std::string BuildImGuiIniPath()
{
    return (EnginePaths::ProjectRoot() / "imgui.ini").string();
}

std::filesystem::path FindFirstExistingFont(std::span<const char* const> candidates)
{
    for (const char* candidate : candidates)
    {
        std::error_code errorCode;
        if (std::filesystem::exists(candidate, errorCode) && !errorCode)
        {
            return std::filesystem::path(candidate);
        }
    }

    return {};
}

std::filesystem::path FindPreferredUiFontPath()
{
#if defined(_WIN32)
    static constexpr std::array<const char*, 4> kCandidates = {
        "C:/Windows/Fonts/segoeuivariable.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/tahoma.ttf"};
#elif defined(__APPLE__)
    static constexpr std::array<const char*, 3> kCandidates = {
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/HelveticaNeue.ttc",
        "/System/Library/Fonts/Supplemental/Arial.ttf"};
#else
    static constexpr std::array<const char*, 4> kCandidates = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/opentype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"};
#endif

    return FindFirstExistingFont(kCandidates);
}

// Merged behind the UI font so Chinese, Japanese and Korean file, model and material names render
// instead of '?'. ImGui 1.92 rasterizes glyphs on first use, so the atlas only grows by the
// characters actually shown.
std::filesystem::path FindCjkFallbackFontPath()
{
#if defined(_WIN32)
    static constexpr std::array<const char*, 3> kCandidates = {
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/msyh.ttf",
        "C:/Windows/Fonts/simhei.ttf"};
#elif defined(__APPLE__)
    static constexpr std::array<const char*, 3> kCandidates = {
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/STHeiti Medium.ttc",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"};
#else
    static constexpr std::array<const char*, 4> kCandidates = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc"};
#endif

    return FindFirstExistingFont(kCandidates);
}

void ConfigureImGuiStyle()
{
    ImGui::StyleColorsDark();

    // Spacing is the Claude desktop app's at its default density (the .cds-root tokens, which its rows
    // and fields measure to): 13 px text in 24 px controls, padding and gaps from its pad and gap scales.
    constexpr float kControlHeight = 24.0f; // --cds-h-control
    constexpr float kNestedControlHeight = 18.0f; // --cds-h-control-nested
    constexpr float kPadXs = 4.0f; // --cds-pad-xs
    constexpr float kPadMd = 8.0f; // --cds-pad-md
    constexpr float kPadLg = 12.0f; // --cds-pad-lg
    constexpr float kGapXs = 6.0f; // --cds-gap-xs
    constexpr float kGapSm = 8.0f; // --cds-gap-sm
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(kPadLg, kPadMd); // --cds-panel-inset vertically
    style.FramePadding = ImVec2(kPadMd, 0.5f * (kControlHeight - kDefaultUiFontSizePixels));
    style.CellPadding = ImVec2(kPadMd, kPadXs);
    style.ItemSpacing = ImVec2(kGapSm, kGapXs);
    style.ItemInnerSpacing = ImVec2(kGapXs, kGapXs);
    style.IndentSpacing = kNestedControlHeight;
    style.ScrollbarSize = 10.0f; // the app's thin overlay scrollbars
    style.GrabMinSize = 14.0f; // --cds-switch-h, the app's smallest knob
    // Corner radii are the Claude desktop app's (its default density, measured on its own rows and
    // fields): controls --cds-radius 6 px, cards and popovers --cds-radius-card (radius + 4 px), and
    // scrollbars fully round. A slider's grab sits 2 px inside its frame, so its corner is the frame's
    // less 2 px, and the two curves stay parallel.
    constexpr float kControlRadius = 6.0f; // --cds-radius
    constexpr float kCardRadius = kControlRadius + 4.0f; // --cds-radius-card
    style.WindowRounding = kCardRadius;
    style.ChildRounding = kCardRadius;
    style.PopupRounding = kCardRadius;
    style.FrameRounding = kControlRadius;
    style.GrabRounding = kControlRadius - 2.0f;
    style.TabRounding = kControlRadius;
    style.ScrollbarRounding = 0.5f * style.ScrollbarSize;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;
    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.SeparatorTextBorderSize = 1.0f;
    style.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
    style.DisplaySafeAreaPadding = ImVec2(kGapXs, kGapXs);
    style.DockingSeparatorSize = 1.0f; // the app's 1 px dividers

    // Colours are the Claude desktop app's dark theme, its design tokens (--cds-*) resolved to sRGB and
    // checked against the app's own pixels: content on surface-1, the sidebar (here the menu bar, the
    // dock's tab bars and empty dock space) one step darker on neutral-30, translucent white fills for
    // fields, buttons and rows, and blue (fill-accent) for the controls the app marks as active.
    // Clay (fill-brand) is the brand colour there, not a control colour, so only plots use it.
    auto rgb = [](int r, int g, int b, float a = 1.0f)
    {
        return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, a);
    };
    auto white = [](float a)
    {
        return ImVec4(1.0f, 1.0f, 1.0f, a);
    };
    auto withAlpha = [](ImVec4 colour, float a)
    {
        colour.w = a;
        return colour;
    };

    const ImVec4 sidebar = rgb(17, 17, 17);            // --cds-neutral-30 (gray-870), the app's sidebar
    const ImVec4 surface1 = rgb(21, 21, 21);           // --cds-surface-1, the app's content area
    const ImVec4 surface3 = rgb(32, 32, 31);           // --cds-surface-3 / surface-popover
    const ImVec4 textPrimary = rgb(240, 239, 236);     // --cds-text-primary
    const ImVec4 textSecondary = rgb(195, 194, 183);   // --cds-text-secondary
    const ImVec4 textMuted = rgb(137, 135, 129);       // --cds-text-muted
    const ImVec4 textAccent = rgb(109, 167, 236);      // --cds-text-accent
    const ImVec4 fillAccent = rgb(42, 120, 214);       // --cds-fill-accent
    const ImVec4 fillAccentHover = rgb(57, 135, 229);  // --cds-fill-accent-hover
    const ImVec4 fillBrandHover = rgb(217, 119, 87);   // --cds-fill-brand-hover (clay)
    const ImVec4 fillWarning = rgb(250, 178, 25);      // --cds-fill-warning
    const ImVec4 fillWarningHover = rgb(237, 161, 0);  // --cds-fill-warning-hover
    const ImVec4 clear = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = textPrimary;
    colors[ImGuiCol_TextDisabled] = textMuted;
    colors[ImGuiCol_WindowBg] = surface1;
    colors[ImGuiCol_ChildBg] = clear;
    colors[ImGuiCol_PopupBg] = surface3;
    colors[ImGuiCol_Border] = white(0.10f); // --cds-alpha-2, the app's dividers and card edges
    colors[ImGuiCol_BorderShadow] = clear;
    colors[ImGuiCol_FrameBg] = white(0.05f);         // --cds-fill-field
    colors[ImGuiCol_FrameBgHovered] = white(0.075f); // --cds-fill-ghost-hover
    colors[ImGuiCol_FrameBgActive] = white(0.10f);   // --cds-fill-control
    colors[ImGuiCol_TitleBg] = sidebar;
    colors[ImGuiCol_TitleBgActive] = sidebar;
    colors[ImGuiCol_TitleBgCollapsed] = sidebar;
    colors[ImGuiCol_MenuBarBg] = sidebar;
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
    colors[ImGuiCol_Header] = white(0.15f);         // --cds-fill-ghost-selected, the app's selected row
    colors[ImGuiCol_HeaderHovered] = white(0.075f); // --cds-fill-ghost-hover
    colors[ImGuiCol_HeaderActive] = white(0.15f);   // --cds-fill-ghost-selected
    colors[ImGuiCol_Separator] = white(0.10f);
    colors[ImGuiCol_SeparatorHovered] = white(0.40f); // --cds-border-stronger
    colors[ImGuiCol_SeparatorActive] = fillAccent;
    colors[ImGuiCol_ResizeGrip] = white(0.10f);
    colors[ImGuiCol_ResizeGripHovered] = white(0.20f);
    colors[ImGuiCol_ResizeGripActive] = fillAccent;
    colors[ImGuiCol_InputTextCursor] = textPrimary;
    // Tabs sit on the sidebar tone and the selected one takes the content's, like the app's sidebar
    // and conversation; the app draws no coloured line on a selected tab.
    colors[ImGuiCol_TabHovered] = white(0.075f);
    colors[ImGuiCol_Tab] = sidebar;
    colors[ImGuiCol_TabSelected] = surface1;
    colors[ImGuiCol_TabSelectedOverline] = clear;
    colors[ImGuiCol_TabDimmed] = sidebar;
    colors[ImGuiCol_TabDimmedSelected] = surface1;
    colors[ImGuiCol_TabDimmedSelectedOverline] = clear;
    colors[ImGuiCol_DockingPreview] = withAlpha(fillAccent, 0.20f); // --cds-shadow-drop-glow
    colors[ImGuiCol_DockingEmptyBg] = sidebar;
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
    colors[ImGuiCol_TextSelectedBg] = withAlpha(fillAccent, 0.35f);
    colors[ImGuiCol_TreeLines] = white(0.20f);
    colors[ImGuiCol_DragDropTarget] = fillAccent; // --cds-shadow-drop-ring
    colors[ImGuiCol_DragDropTargetBg] = clear;
    colors[ImGuiCol_UnsavedMarker] = textPrimary;
    colors[ImGuiCol_NavCursor] = fillAccent;
    colors[ImGuiCol_NavWindowingHighlight] = white(0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.50f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.50f); // --cds-backdrop
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

    ImFont* defaultFont = nullptr;
    const std::filesystem::path preferredFontPath = FindPreferredUiFontPath();
    if (!preferredFontPath.empty())
    {
        const std::string preferredFontPathString = preferredFontPath.string();
        defaultFont = fonts->AddFontFromFileTTF(preferredFontPathString.c_str(), fontConfig.SizePixels, &fontConfig);
    }

    if (defaultFont == nullptr)
    {
        defaultFont = fonts->AddFontDefaultVector(&fontConfig);
    }

    if (const std::filesystem::path cjkFontPath = FindCjkFallbackFontPath(); !cjkFontPath.empty())
    {
        ImFontConfig cjkConfig{};
        cjkConfig.MergeMode = true;
        cjkConfig.SizePixels = fontConfig.SizePixels;
        cjkConfig.OversampleH = fontConfig.OversampleH;
        cjkConfig.OversampleV = fontConfig.OversampleV;
        cjkConfig.PixelSnapH = fontConfig.PixelSnapH;
        cjkConfig.RasterizerMultiply = fontConfig.RasterizerMultiply;
        const std::string cjkFontPathString = cjkFontPath.string();
        fonts->AddFontFromFileTTF(cjkFontPathString.c_str(), cjkConfig.SizePixels, &cjkConfig);
    }

    MergeEditorIconFont(*fonts, fontConfig.SizePixels);

    io.FontDefault = defaultFont;
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
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = m_iniFilePath.c_str();
    // The layout (positions, sizes, docking) is written a second after it changes rather than ImGui's
    // default five, so a crash or a killed process loses little of it.
    io.IniSavingRate = 1.0f;
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
    ImGui_ImplSDL3_Shutdown();
    ImPlot::DestroyContext();
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

void VulkanImGuiLayer::CreateOrUpdateVulkanResources(VkRenderPass renderPass, uint32_t imageCount, bool hdrOutput)
{
    DestroyVulkanResources();

    m_hdrFragmentShader.clear();
    if (hdrOutput)
    {
        const std::filesystem::path path = EnginePaths::ShaderRoot() / "imgui_hdr10.frag.spv";
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
        {
            throw std::runtime_error("Failed to open " + path.string());
        }
        const std::streamsize size = file.tellg();
        m_hdrFragmentShader.resize(static_cast<size_t>(size) / sizeof(uint32_t));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(m_hdrFragmentShader.data()), size);
    }

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = VK_API_VERSION_1_3;
    initInfo.Instance = m_instance;
    initInfo.PhysicalDevice = m_physicalDevice;
    initInfo.Device = m_device;
    initInfo.QueueFamily = m_graphicsQueueFamily;
    initInfo.Queue = m_graphicsQueue;
    initInfo.DescriptorPool = m_descriptorPool;
    initInfo.PipelineInfoMain.RenderPass = renderPass;
    initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.MinImageCount = imageCount;
    initInfo.ImageCount = imageCount;
    initInfo.CheckVkResultFn = &VulkanImGuiLayer::CheckVkResult;
    if (!m_hdrFragmentShader.empty())
    {
        initInfo.CustomShaderFragCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        initInfo.CustomShaderFragCreateInfo.codeSize = m_hdrFragmentShader.size() * sizeof(uint32_t);
        initInfo.CustomShaderFragCreateInfo.pCode = m_hdrFragmentShader.data();
    }

    // The font atlas is uploaded by the backend itself (ImGuiBackendFlags_RendererHasTextures): glyphs
    // are rasterised at the size they are drawn, so scaled text and icons stay sharp.
    if (!ImGui_ImplVulkan_Init(&initInfo))
    {
        throw std::runtime_error("Failed to initialize ImGui Vulkan backend");
    }

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
    // ImGui binds the image (set 0) and the sampler (set 1, its linear and nearest samplers) separately.
    const std::array<VkDescriptorPoolSize, 2> poolSizes = {{
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kImGuiDescriptorCount},
        {VK_DESCRIPTOR_TYPE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE},
    }};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = kImGuiDescriptorCount + IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();

    CheckVulkan(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool), "Failed to create ImGui descriptor pool");
}

void VulkanImGuiLayer::CheckVkResult(VkResult result)
{
    CheckVulkan(result, "ImGui Vulkan backend call failed");
}
}
