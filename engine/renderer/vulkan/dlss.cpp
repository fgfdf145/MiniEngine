#include "dlss.h"

#include <array>

#include <engine/core/log/log.h>

#if MINIENGINE_WITH_DLSS
#include "upload_batch.h"

#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>
// After the Vulkan helpers, whose types and macros it uses.
#include <nvsdk_ngx_helpers_dlssd_vk.h>

#include <filesystem>
#include <format>
#endif

namespace me
{

#if MINIENGINE_WITH_DLSS
namespace
{
// NGX identifies an application without an NVIDIA application ID by a GUID-like project ID, the engine
// type and its version (DLSS programming guide 5.2.1).
constexpr const char* kProjectId = "765f0055-83ee-4709-8004-8ca25b87b97b";
constexpr const char* kEngineVersion = "0.1";

// Where NGX writes its logs and caches; it must be writable or NGX may fail to start (guide 3.19).
const std::wstring& DataDirectory()
{
    static const std::wstring directory = []()
    {
        std::error_code error;
        std::filesystem::path path = std::filesystem::temp_directory_path(error) / "MiniEngine" / "ngx";
        std::filesystem::create_directories(path, error);
        return path.wstring();
    }();
    return directory;
}

void NVSDK_CONV LogFromNgx(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
    std::string line(message != nullptr ? message : "");
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
    {
        line.pop_back();
    }
    LOG_INFO("NGX: {}", line);
}

NVSDK_NGX_FeatureCommonInfo CommonInfo()
{
    NVSDK_NGX_FeatureCommonInfo info{};
    info.LoggingInfo.LoggingCallback = &LogFromNgx;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    info.LoggingInfo.DisableOtherLoggingSinks = false;
    return info;
}

NVSDK_NGX_FeatureDiscoveryInfo DiscoveryInfo(const NVSDK_NGX_FeatureCommonInfo& common)
{
    NVSDK_NGX_FeatureDiscoveryInfo info{};
    info.SDKVersion = NVSDK_NGX_Version_API;
    info.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    info.Identifier.v.ProjectDesc.ProjectId = kProjectId;
    info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    info.Identifier.v.ProjectDesc.EngineVersion = kEngineVersion;
    info.ApplicationDataPath = DataDirectory().c_str();
    info.FeatureInfo = &common;
    return info;
}

std::vector<std::string> ExtensionNames(uint32_t count, const VkExtensionProperties* properties)
{
    std::vector<std::string> names;
    for (uint32_t index = 0; index < count && properties != nullptr; ++index)
    {
        names.emplace_back(properties[index].extensionName);
    }
    return names;
}

std::string ResultText(NVSDK_NGX_Result result)
{
    const wchar_t* text = GetNGXResultAsString(result);
    std::string narrow;
    for (const wchar_t* c = text; c != nullptr && *c != L'\0'; ++c)
    {
        narrow.push_back(*c < 128 ? static_cast<char>(*c) : '?');
    }
    return std::format("{} (0x{:08x})", narrow, static_cast<uint32_t>(result));
}

NVSDK_NGX_PerfQuality_Value ToPerfQuality(DlssMode mode)
{
    switch (mode)
    {
    case DlssMode::Dlaa:
        return NVSDK_NGX_PerfQuality_Value_DLAA;
    case DlssMode::Quality:
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case DlssMode::Balanced:
        return NVSDK_NGX_PerfQuality_Value_Balanced;
    case DlssMode::Performance:
        return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case DlssMode::UltraPerformance:
        return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    case DlssMode::Off:
        break;
    }
    return NVSDK_NGX_PerfQuality_Value_MaxQuality;
}

NVSDK_NGX_Resource_VK ToResource(const DlssImage& image, bool readWrite)
{
    const VkImageSubresourceRange range{image.aspect, 0, 1, 0, 1};
    return NVSDK_NGX_Create_ImageView_Resource_VK(
        image.view,
        image.image,
        range,
        image.format,
        image.extent.width,
        image.extent.height,
        readWrite);
}

NVSDK_NGX_DLSS_Hint_Render_Preset ToRenderPreset(DlssPreset preset)
{
    switch (preset)
    {
    case DlssPreset::J:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_J;
    case DlssPreset::K:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
    case DlssPreset::L:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_L;
    case DlssPreset::M:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_M;
    case DlssPreset::Default:
        break;
    }
    return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
}

const char* PresetName(DlssPreset preset)
{
    switch (preset)
    {
    case DlssPreset::J:
        return "J";
    case DlssPreset::K:
        return "K";
    case DlssPreset::L:
        return "L";
    case DlssPreset::M:
        return "M";
    case DlssPreset::Default:
        break;
    }
    return "default";
}

// NGX reads one preset hint per quality when it makes a feature; the parameters outlive features, so
// every hint is set each time, Default included, rather than only the mode's own.
void SetRenderPresetHints(NVSDK_NGX_Parameter* parameters, DlssPreset preset)
{
    const unsigned int value = ToRenderPreset(preset);
    for (const char* name : {
             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
    {
        NVSDK_NGX_Parameter_SetUI(parameters, name, value);
    }
}
}

namespace
{
// One DlssFeatureSlot's feature and what it was made for.
struct DlssSlotState
{
    NVSDK_NGX_Handle* feature = nullptr;
    VkExtent2D featureRender{};
    VkExtent2D featureOutput{};
    DlssMode featureMode = DlssMode::Off;
    DlssPreset featurePreset = DlssPreset::Default;
    bool featureRayReconstruction = false;
    // The last feature NGX would not make, so a failing size is not retried every frame.
    VkExtent2D failedRender{};
    VkExtent2D failedOutput{};
    DlssMode failedMode = DlssMode::Off;
    DlssPreset failedPreset = DlssPreset::Default;
    bool failedRayReconstruction = false;
    // The last optimal-settings answer, asked again only when the output size or mode changes.
    VkExtent2D optimalOutput{};
    DlssMode optimalMode = DlssMode::Off;
    std::optional<VkExtent2D> optimalRender;
};
}

struct VulkanDlss::Ngx
{
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    bool initialized = false;
    NVSDK_NGX_Parameter* parameters = nullptr;
    std::array<DlssSlotState, kDlssFeatureSlotCount> slots;

    DlssSlotState& Slot(DlssFeatureSlot slot)
    {
        return slots.at(static_cast<uint32_t>(slot));
    }
};

namespace
{
bool SameExtent(VkExtent2D a, VkExtent2D b)
{
    return a.width == b.width && a.height == b.height;
}
}

std::vector<std::string> VulkanDlss::RequiredInstanceExtensions()
{
    const NVSDK_NGX_FeatureCommonInfo common = CommonInfo();
    const NVSDK_NGX_FeatureDiscoveryInfo discovery = DiscoveryInfo(common);
    uint32_t count = 0;
    VkExtensionProperties* properties = nullptr;
    const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&discovery, &count, &properties);
    if (NVSDK_NGX_FAILED(result))
    {
        LOG_WARN("DLSS: no instance extension list ({})", ResultText(result));
        return {};
    }
    return ExtensionNames(count, properties);
}

std::vector<std::string> VulkanDlss::RequiredDeviceExtensions(VkInstance instance, VkPhysicalDevice physicalDevice)
{
    const NVSDK_NGX_FeatureCommonInfo common = CommonInfo();
    const NVSDK_NGX_FeatureDiscoveryInfo discovery = DiscoveryInfo(common);
    uint32_t count = 0;
    VkExtensionProperties* properties = nullptr;
    const NVSDK_NGX_Result result =
        NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, physicalDevice, &discovery, &count, &properties);
    if (NVSDK_NGX_FAILED(result))
    {
        LOG_WARN("DLSS: no device extension list ({})", ResultText(result));
        return {};
    }
    return ExtensionNames(count, properties);
}

VulkanDlss::VulkanDlss(
    VkInstance instance,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    uint32_t graphicsQueueFamily,
    VkQueue graphicsQueue,
    bool extensionsEnabled)
    : m_ngx(std::make_unique<Ngx>())
{
    m_ngx->device = device;
    m_ngx->queueFamily = graphicsQueueFamily;
    m_ngx->queue = graphicsQueue;
    if (!extensionsEnabled)
    {
        m_status = "the device lacks extensions DLSS needs";
        LOG_INFO("DLSS unavailable: {}", m_status);
        return;
    }

    const NVSDK_NGX_FeatureCommonInfo common = CommonInfo();
    NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        kProjectId,
        NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        kEngineVersion,
        DataDirectory().c_str(),
        instance,
        physicalDevice,
        device,
        vkGetInstanceProcAddr,
        vkGetDeviceProcAddr,
        &common);
    if (NVSDK_NGX_FAILED(result))
    {
        m_status = std::format("NGX did not start: {}", ResultText(result));
        LOG_INFO("DLSS unavailable: {}", m_status);
        return;
    }
    m_ngx->initialized = true;

    result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&m_ngx->parameters);
    if (NVSDK_NGX_FAILED(result) || m_ngx->parameters == nullptr)
    {
        m_status = std::format("no NGX capability parameters: {}", ResultText(result));
        LOG_INFO("DLSS unavailable: {}", m_status);
        return;
    }

    int needsUpdatedDriver = 0;
    unsigned int minDriverMajor = 0;
    unsigned int minDriverMinor = 0;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(m_ngx->parameters, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsUpdatedDriver)) &&
        needsUpdatedDriver != 0)
    {
        NVSDK_NGX_Parameter_GetUI(m_ngx->parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minDriverMajor);
        NVSDK_NGX_Parameter_GetUI(m_ngx->parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minDriverMinor);
        m_status = std::format("the driver is too old, DLSS needs {}.{}", minDriverMajor, minDriverMinor);
        LOG_INFO("DLSS unavailable: {}", m_status);
        return;
    }
    int available = 0;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetI(m_ngx->parameters, NVSDK_NGX_Parameter_SuperSampling_Available, &available)) ||
        available == 0)
    {
        int initResult = 0;
        NVSDK_NGX_Parameter_GetI(m_ngx->parameters, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
        m_status = std::format("not supported here: {}", ResultText(static_cast<NVSDK_NGX_Result>(initResult)));
        LOG_INFO("DLSS unavailable: {}", m_status);
        return;
    }
    // A feature released for a new size gives its memory back rather than keeping it for the next.
    NVSDK_NGX_Parameter_SetI(m_ngx->parameters, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
    m_available = true;

    // Ray reconstruction (DLSS-D): its own capability, driver and runtime (nvngx_dlssd).
    int rayReconstruction = 0;
    int rayReconstructionNeedsDriver = 0;
    NVSDK_NGX_Parameter_GetI(m_ngx->parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver, &rayReconstructionNeedsDriver);
    m_rayReconstructionAvailable =
        NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(m_ngx->parameters, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &rayReconstruction)) &&
        rayReconstruction != 0 && rayReconstructionNeedsDriver == 0;
    m_status = m_rayReconstructionAvailable ? "available, with ray reconstruction" : "available (no ray reconstruction)";
    LOG_INFO("DLSS available; ray reconstruction {}", m_rayReconstructionAvailable ? "available" : "unavailable");
}

VulkanDlss::~VulkanDlss()
{
    for (uint32_t slot = 0; slot < kDlssFeatureSlotCount; ++slot)
    {
        ReleaseFeature(static_cast<DlssFeatureSlot>(slot));
    }
    if (m_ngx->parameters != nullptr)
    {
        NVSDK_NGX_VULKAN_DestroyParameters(m_ngx->parameters);
    }
    if (m_ngx->initialized)
    {
        NVSDK_NGX_VULKAN_Shutdown1(m_ngx->device);
    }
}

bool VulkanDlss::IsAvailable() const
{
    return m_available;
}

bool VulkanDlss::IsRayReconstructionAvailable() const
{
    return m_rayReconstructionAvailable;
}

const std::string& VulkanDlss::Status() const
{
    return m_status;
}

std::optional<VkExtent2D> VulkanDlss::RenderExtentFor(VkExtent2D output, DlssMode mode, DlssFeatureSlot slot)
{
    DlssSlotState& f = m_ngx->Slot(slot);
    if (!IsAvailable() || mode == DlssMode::Off || output.width < 32 || output.height < 32)
    {
        return std::nullopt;
    }
    if (mode == DlssMode::Dlaa)
    {
        return output;
    }
    if (mode == f.optimalMode && SameExtent(output, f.optimalOutput))
    {
        return f.optimalRender;
    }
    f.optimalMode = mode;
    f.optimalOutput = output;
    f.optimalRender.reset();
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int maxWidth = 0;
    unsigned int maxHeight = 0;
    unsigned int minWidth = 0;
    unsigned int minHeight = 0;
    float sharpness = 0.0f;
    const NVSDK_NGX_Result result = NGX_DLSS_GET_OPTIMAL_SETTINGS(
        m_ngx->parameters,
        output.width,
        output.height,
        ToPerfQuality(mode),
        &width,
        &height,
        &maxWidth,
        &maxHeight,
        &minWidth,
        &minHeight,
        &sharpness);
    if (NVSDK_NGX_FAILED(result) || width == 0 || height == 0)
    {
        LOG_WARN("DLSS: no optimal settings for {}x{} ({})", output.width, output.height, ResultText(result));
        return std::nullopt;
    }
    f.optimalRender = VkExtent2D{width, height};
    return f.optimalRender;
}

bool VulkanDlss::EnsureFeature(VkExtent2D render, VkExtent2D output, DlssMode mode, DlssPreset preset, bool rayReconstruction, DlssFeatureSlot slot)
{
    DlssSlotState& f = m_ngx->Slot(slot);
    if (!IsAvailable())
    {
        return false;
    }
    rayReconstruction = rayReconstruction && m_rayReconstructionAvailable;
    if (f.feature != nullptr && f.featureMode == mode && f.featurePreset == preset &&
        f.featureRayReconstruction == rayReconstruction &&
        SameExtent(f.featureRender, render) && SameExtent(f.featureOutput, output))
    {
        return true;
    }
    if (f.failedMode == mode && f.failedPreset == preset && f.failedRayReconstruction == rayReconstruction &&
        SameExtent(f.failedRender, render) &&
        SameExtent(f.failedOutput, output))
    {
        return false;
    }
    ReleaseFeature(slot);

    // Pre-exposed linear HDR (pre_exposure.glsl), reverse-Z depth, motion vectors at the render size
    // without the jitter; DLSS meters the exposure itself, which presets L and M always do.
    const int createFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                            NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                            NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                            NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    if (rayReconstruction)
    {
        NVSDK_NGX_DLSSD_Create_Params create{};
        create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
        // The roughness rides in the normals' w (dlss_rr_guides.comp); the depth is the hardware one.
        create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
        create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
        create.InWidth = render.width;
        create.InHeight = render.height;
        create.InTargetWidth = output.width;
        create.InTargetHeight = output.height;
        create.InPerfQualityValue = ToPerfQuality(mode);
        create.InFeatureCreateFlags = createFlags;
        VulkanUploadBatch batch(m_ngx->device, m_ngx->queueFamily, m_ngx->queue);
        const NVSDK_NGX_Result result =
            NGX_VULKAN_CREATE_DLSSD_EXT1(m_ngx->device, batch.GetCommandBuffer(), 1, 1, &f.feature, m_ngx->parameters, &create);
        batch.Flush();
        if (NVSDK_NGX_FAILED(result) || f.feature == nullptr)
        {
            f.feature = nullptr;
            f.failedRender = render;
            f.failedOutput = output;
            f.failedMode = mode;
            f.failedPreset = preset;
            f.failedRayReconstruction = true;
            LOG_WARN(
                "DLSS: could not create ray reconstruction for {}x{} -> {}x{} ({})",
                render.width,
                render.height,
                output.width,
                output.height,
                ResultText(result));
            return false;
        }
        f.featureRender = render;
        f.featureOutput = output;
        f.featureMode = mode;
        f.featurePreset = preset;
        f.featureRayReconstruction = true;
        LOG_INFO("DLSS ray reconstruction: {}x{} -> {}x{}", render.width, render.height, output.width, output.height);
        return true;
    }

    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = render.width;
    create.Feature.InHeight = render.height;
    create.Feature.InTargetWidth = output.width;
    create.Feature.InTargetHeight = output.height;
    create.Feature.InPerfQualityValue = ToPerfQuality(mode);
    create.InFeatureCreateFlags = createFlags;
    SetRenderPresetHints(m_ngx->parameters, preset);

    VulkanUploadBatch batch(m_ngx->device, m_ngx->queueFamily, m_ngx->queue);
    const NVSDK_NGX_Result result =
        NGX_VULKAN_CREATE_DLSS_EXT1(m_ngx->device, batch.GetCommandBuffer(), 1, 1, &f.feature, m_ngx->parameters, &create);
    batch.Flush();
    if (NVSDK_NGX_FAILED(result) || f.feature == nullptr)
    {
        f.feature = nullptr;
        f.failedRender = render;
        f.failedOutput = output;
        f.failedMode = mode;
        f.failedPreset = preset;
        f.failedRayReconstruction = false;
        LOG_WARN(
            "DLSS: could not create the feature for {}x{} -> {}x{} ({})",
            render.width,
            render.height,
            output.width,
            output.height,
            ResultText(result));
        return false;
    }
    f.featureRender = render;
    f.featureOutput = output;
    f.featureMode = mode;
    f.featurePreset = preset;
    f.featureRayReconstruction = false;
    LOG_INFO("DLSS: {}x{} -> {}x{}, preset {}", render.width, render.height, output.width, output.height, PresetName(preset));
    return true;
}

void VulkanDlss::ReleaseFeature(DlssFeatureSlot slot)
{
    DlssSlotState& f = m_ngx->Slot(slot);
    if (f.feature != nullptr)
    {
        // Frames still in flight may evaluate it.
        vkDeviceWaitIdle(m_ngx->device);
        NVSDK_NGX_VULKAN_ReleaseFeature(f.feature);
        f.feature = nullptr;
    }
    f.featureMode = DlssMode::Off;
    f.featureRayReconstruction = false;
}

bool VulkanDlss::HasFeature(DlssFeatureSlot slot) const
{
    const DlssSlotState& f = m_ngx->Slot(slot);
    return f.feature != nullptr;
}

bool VulkanDlss::HasRayReconstruction(DlssFeatureSlot slot) const
{
    const DlssSlotState& f = m_ngx->Slot(slot);
    return f.feature != nullptr && f.featureRayReconstruction;
}

bool VulkanDlss::Evaluate(VkCommandBuffer commandBuffer, const DlssEvaluateInputs& inputs, DlssFeatureSlot slot)
{
    DlssSlotState& f = m_ngx->Slot(slot);
    if (f.feature == nullptr)
    {
        return false;
    }
    NVSDK_NGX_Resource_VK color = ToResource(inputs.color, false);
    NVSDK_NGX_Resource_VK depth = ToResource(inputs.depth, false);
    NVSDK_NGX_Resource_VK motion = ToResource(inputs.motionVectors, false);
    NVSDK_NGX_Resource_VK output = ToResource(inputs.output, true);

    if (f.featureRayReconstruction)
    {
        NVSDK_NGX_Resource_VK diffuseAlbedo = ToResource(inputs.diffuseAlbedo, false);
        NVSDK_NGX_Resource_VK specularAlbedo = ToResource(inputs.specularAlbedo, false);
        NVSDK_NGX_Resource_VK normalRoughness = ToResource(inputs.normalRoughness, false);
        NVSDK_NGX_Resource_VK specularHitDistance{};
        NVSDK_NGX_Resource_VK reflectionMotion{};
        const bool hasHitDistance = inputs.specularHitDistance.image != VK_NULL_HANDLE && inputs.reflectionMotionVectors.image != VK_NULL_HANDLE;
        if (hasHitDistance)
        {
            specularHitDistance = ToResource(inputs.specularHitDistance, false);
            reflectionMotion = ToResource(inputs.reflectionMotionVectors, false);
        }
        // NGX takes row-major matrices for row vectors, which is glm's column-major storage as it is.
        glm::mat4 worldToView = inputs.worldToView;
        glm::mat4 viewToClip = inputs.viewToClip;
        NVSDK_NGX_VK_DLSSD_Eval_Params evaluate{};
        evaluate.pInColor = &color;
        evaluate.pInOutput = &output;
        evaluate.pInDepth = &depth;
        evaluate.pInMotionVectors = &motion;
        evaluate.pInDiffuseAlbedo = &diffuseAlbedo;
        evaluate.pInSpecularAlbedo = &specularAlbedo;
        evaluate.pInNormals = &normalRoughness;
        if (hasHitDistance)
        {
            evaluate.pInSpecularHitDistance = &specularHitDistance;
            evaluate.pInMotionVectorsReflections = &reflectionMotion;
        }
        evaluate.InJitterOffsetX = inputs.jitterPixels.x;
        evaluate.InJitterOffsetY = inputs.jitterPixels.y;
        evaluate.InRenderSubrectDimensions = {inputs.color.extent.width, inputs.color.extent.height};
        evaluate.InReset = inputs.reset ? 1 : 0;
        evaluate.InMVScaleX = 1.0f;
        evaluate.InMVScaleY = 1.0f;
        evaluate.InFrameTimeDeltaInMsec = inputs.frameTimeMs;
        evaluate.pInWorldToViewMatrix = &worldToView[0][0];
        evaluate.pInViewToClipMatrix = &viewToClip[0][0];
        const NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSSD_EXT(commandBuffer, f.feature, m_ngx->parameters, &evaluate);
        if (NVSDK_NGX_FAILED(result))
        {
            LOG_WARN("DLSS ray reconstruction: evaluation failed ({})", ResultText(result));
            return false;
        }
        return true;
    }

    NVSDK_NGX_VK_DLSS_Eval_Params evaluate{};
    evaluate.Feature.pInColor = &color;
    evaluate.Feature.pInOutput = &output;
    evaluate.pInDepth = &depth;
    evaluate.pInMotionVectors = &motion;
    evaluate.InJitterOffsetX = inputs.jitterPixels.x;
    evaluate.InJitterOffsetY = inputs.jitterPixels.y;
    evaluate.InRenderSubrectDimensions = {inputs.color.extent.width, inputs.color.extent.height};
    evaluate.InReset = inputs.reset ? 1 : 0;
    evaluate.InMVScaleX = 1.0f;
    evaluate.InMVScaleY = 1.0f;
    evaluate.InFrameTimeDeltaInMsec = inputs.frameTimeMs;
    const NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSS_EXT(commandBuffer, f.feature, m_ngx->parameters, &evaluate);
    if (NVSDK_NGX_FAILED(result))
    {
        LOG_WARN("DLSS: evaluation failed ({})", ResultText(result));
        return false;
    }
    return true;
}

#else

// Built without the SDK: DLSS never becomes available.
struct VulkanDlss::Ngx
{
};

std::vector<std::string> VulkanDlss::RequiredInstanceExtensions()
{
    return {};
}

std::vector<std::string> VulkanDlss::RequiredDeviceExtensions(VkInstance, VkPhysicalDevice)
{
    return {};
}

VulkanDlss::VulkanDlss(VkInstance, VkPhysicalDevice, VkDevice, uint32_t, VkQueue, bool)
    : m_ngx(std::make_unique<Ngx>()),
      m_status("built without the DLSS SDK (scripts/fetch-dlss-sdk.sh)")
{
}

VulkanDlss::~VulkanDlss() = default;

bool VulkanDlss::IsAvailable() const
{
    return false;
}

const std::string& VulkanDlss::Status() const
{
    return m_status;
}

std::optional<VkExtent2D> VulkanDlss::RenderExtentFor(VkExtent2D, DlssMode, DlssFeatureSlot)
{
    return std::nullopt;
}

bool VulkanDlss::IsRayReconstructionAvailable() const
{
    return false;
}

bool VulkanDlss::EnsureFeature(VkExtent2D, VkExtent2D, DlssMode, DlssPreset, bool, DlssFeatureSlot)
{
    return false;
}

bool VulkanDlss::HasRayReconstruction(DlssFeatureSlot) const
{
    return false;
}

void VulkanDlss::ReleaseFeature(DlssFeatureSlot)
{
}

bool VulkanDlss::HasFeature(DlssFeatureSlot) const
{
    return false;
}

bool VulkanDlss::Evaluate(VkCommandBuffer, const DlssEvaluateInputs&, DlssFeatureSlot)
{
    return false;
}

#endif
}
