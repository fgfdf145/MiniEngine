#include "renderer.h"

#include "../imgui/imgui_impl_metal.h"
#include "../imgui/imgui_impl_sdl3.h"
#include "../texture_loader.h"

#include <imgui.h>
#include <log/log.h>
#include <mesh.h>
#include <window/window.h>

#include <SDL3/SDL_metal.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <simd/simd.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace
{
constexpr float kDefaultUiFontSizePixels = 16.0f;
constexpr MTLPixelFormat kSceneColorFormat = MTLPixelFormatBGRA8Unorm;
constexpr MTLPixelFormat kSceneDepthFormat = MTLPixelFormatDepth32Float;

constexpr const char* kSceneShaderSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

constant float kPi = 3.14159265359;
constexpr sampler materialSampler(coord::normalized, address::repeat, filter::linear);

struct VertexInput
{
    float3 position [[attribute(0)]];
    float3 color [[attribute(1)]];
    float2 texCoord [[attribute(2)]];
    float3 normal [[attribute(3)]];
    float4 tangent [[attribute(4)]];
};

struct SceneFrameUniforms
{
    float4x4 viewProjectionMatrix;
    float4 cameraWorldPosition;
    float4 lightDirectionAndIntensity;
    float4 lightColorAndAmbient;
};

struct SceneDrawUniforms
{
    float4x4 modelMatrix;
    float4x4 normalMatrix;
    float4 baseColorFactor;
    float4 emissiveFactor;
    float4 surfaceFactors;
    float4 nodeGraphFactors;
};

struct VertexOutput
{
    float4 position [[position]];
    float3 color;
    float2 texCoord;
    float3 worldNormal;
    float4 worldTangent;
    float3 worldPosition;
};

vertex VertexOutput scene_vertex(
    VertexInput in [[stage_in]],
    constant SceneFrameUniforms& frame [[buffer(1)]],
    constant SceneDrawUniforms& draw [[buffer(2)]]
)
{
    VertexOutput out;
    float4 worldPosition = draw.modelMatrix * float4(in.position, 1.0);
    float3x3 normalMatrix = float3x3(draw.normalMatrix[0].xyz, draw.normalMatrix[1].xyz, draw.normalMatrix[2].xyz);
    float3 geometricNormal = normalize(normalMatrix * in.normal);
    float3 worldTangent = normalize(normalMatrix * in.tangent.xyz);
    out.position = frame.viewProjectionMatrix * worldPosition;
    out.color = in.color;
    out.texCoord = in.texCoord;
    out.worldNormal = geometricNormal;
    out.worldTangent = float4(worldTangent, in.tangent.w);
    out.worldPosition = worldPosition.xyz;
    return out;
}

float distribution_ggx(float3 normal, float3 halfVector, float roughness)
{
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float nDotH = max(dot(normal, halfVector), 0.0);
    float nDotHSquared = nDotH * nDotH;
    float denominator = nDotHSquared * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(kPi * denominator * denominator, 0.0001);
}

float geometry_schlick_ggx(float nDotV, float roughness)
{
    float remappedRoughness = roughness + 1.0;
    float k = (remappedRoughness * remappedRoughness) / 8.0;
    float denominator = nDotV * (1.0 - k) + k;
    return nDotV / max(denominator, 0.0001);
}

float geometry_smith(float3 normal, float3 viewDirection, float3 lightDirection, float roughness)
{
    float nDotV = max(dot(normal, viewDirection), 0.0);
    float nDotL = max(dot(normal, lightDirection), 0.0);
    return geometry_schlick_ggx(nDotV, roughness) * geometry_schlick_ggx(nDotL, roughness);
}

float3 fresnel_schlick(float cosineTheta, float3 baseReflectivity)
{
    return baseReflectivity + (1.0 - baseReflectivity) * pow(1.0 - cosineTheta, 5.0);
}

fragment half4 scene_fragment(
    VertexOutput in [[stage_in]],
    constant SceneFrameUniforms& frame [[buffer(0)]],
    constant SceneDrawUniforms& draw [[buffer(1)]],
    texture2d<float> baseColorTexture [[texture(0)]],
    texture2d<float> normalTexture [[texture(1)]],
    texture2d<float> metallicTexture [[texture(2)]],
    texture2d<float> roughnessTexture [[texture(3)]],
    texture2d<float> occlusionTexture [[texture(4)]],
    texture2d<float> emissiveTexture [[texture(5)]],
    texture2d<float> secondaryBaseColorTexture [[texture(6)]],
    texture2d<float> secondaryNormalTexture [[texture(7)]],
    texture2d<float> secondaryMetallicTexture [[texture(8)]],
    texture2d<float> secondaryRoughnessTexture [[texture(9)]],
    texture2d<float> secondaryOcclusionTexture [[texture(10)]],
    texture2d<float> secondaryEmissiveTexture [[texture(11)]],
    texture2d<float> blendMaskTexture [[texture(12)]]
)
{
    float blendMask = blendMaskTexture.sample(materialSampler, in.texCoord).r;
    float blendWeight = clamp(
        mix(0.0, draw.nodeGraphFactors.y, clamp(draw.nodeGraphFactors.x, 0.0, 1.0)) * blendMask,
        0.0,
        1.0
    );

    float4 primaryBaseColor = baseColorTexture.sample(materialSampler, in.texCoord);
    float4 secondaryBaseColor = secondaryBaseColorTexture.sample(materialSampler, in.texCoord);
    float4 sampledBaseColor = mix(primaryBaseColor, secondaryBaseColor, blendWeight);
    float4 albedo = sampledBaseColor * float4(in.color, 1.0) * draw.baseColorFactor;

    float3 geometricNormal = normalize(in.worldNormal);
    float3 tangent = normalize(in.worldTangent.xyz - geometricNormal * dot(geometricNormal, in.worldTangent.xyz));
    if (length(tangent) < 0.0001)
    {
        tangent = normalize(cross(abs(geometricNormal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0), geometricNormal));
    }
    float3 bitangent = normalize(cross(geometricNormal, tangent) * in.worldTangent.w);
    float3x3 tbn = float3x3(tangent, bitangent, geometricNormal);

    float3 sampledNormalPrimary = normalTexture.sample(materialSampler, in.texCoord).xyz * 2.0 - 1.0;
    float3 sampledNormalSecondary = secondaryNormalTexture.sample(materialSampler, in.texCoord).xyz * 2.0 - 1.0;
    float3 sampledNormal = normalize(mix(sampledNormalPrimary, sampledNormalSecondary, blendWeight));
    sampledNormal.xy *= draw.surfaceFactors.z;
    float3 normal = normalize(tbn * sampledNormal);

    float metallicSample = mix(
        metallicTexture.sample(materialSampler, in.texCoord).b,
        secondaryMetallicTexture.sample(materialSampler, in.texCoord).b,
        blendWeight
    );
    float roughnessSample = mix(
        roughnessTexture.sample(materialSampler, in.texCoord).g,
        secondaryRoughnessTexture.sample(materialSampler, in.texCoord).g,
        blendWeight
    );
    float ambientOcclusionSample = mix(
        occlusionTexture.sample(materialSampler, in.texCoord).r,
        secondaryOcclusionTexture.sample(materialSampler, in.texCoord).r,
        blendWeight
    );
    float3 emissiveSample = mix(
        emissiveTexture.sample(materialSampler, in.texCoord).rgb,
        secondaryEmissiveTexture.sample(materialSampler, in.texCoord).rgb,
        blendWeight
    );

    float metallic = clamp(draw.surfaceFactors.x * metallicSample, 0.0, 1.0);
    float roughness = clamp(draw.surfaceFactors.y * roughnessSample, 0.04, 1.0);
    float ambientOcclusion = mix(1.0, ambientOcclusionSample, clamp(draw.surfaceFactors.w, 0.0, 1.0));

    float3 viewDirection = normalize(frame.cameraWorldPosition.xyz - in.worldPosition);
    float3 lightDirection = normalize(-frame.lightDirectionAndIntensity.xyz);
    float3 halfVector = normalize(viewDirection + lightDirection);
    float3 radiance = frame.lightColorAndAmbient.rgb * frame.lightDirectionAndIntensity.w;

    float nDotL = max(dot(normal, lightDirection), 0.0);
    float nDotV = max(dot(normal, viewDirection), 0.0);
    float hDotV = max(dot(halfVector, viewDirection), 0.0);

    float3 baseReflectivity = mix(float3(0.04), albedo.rgb, metallic);
    float3 fresnel = fresnel_schlick(hDotV, baseReflectivity);
    float distribution = distribution_ggx(normal, halfVector, roughness);
    float geometry = geometry_smith(normal, viewDirection, lightDirection, roughness);

    float3 numerator = distribution * geometry * fresnel;
    float denominator = max(4.0 * nDotV * nDotL, 0.0001);
    float3 specular = numerator / denominator;

    float3 specularRatio = fresnel;
    float3 diffuseRatio = (float3(1.0) - specularRatio) * (1.0 - metallic);
    float3 diffuse = diffuseRatio * albedo.rgb / kPi;

    float3 ambient = albedo.rgb * frame.lightColorAndAmbient.w * ambientOcclusion;
    float3 directLighting = (diffuse + specular) * radiance * nDotL;
    float3 emissive = emissiveSample * draw.emissiveFactor.rgb;

    float3 color = ambient + directLighting + emissive;
    color = color / (color + float3(1.0));

    return half4(half3(color), half(albedo.a));
}
)METAL";

enum class MetalTextureFormat
{
    SrgbColor,
    LinearData
};

struct MetalSceneFrameUniforms
{
    matrix_float4x4 viewProjectionMatrix;
    vector_float4 cameraWorldPosition;
    vector_float4 lightDirectionAndIntensity;
    vector_float4 lightColorAndAmbient;
};

struct MetalSceneDrawUniforms
{
    matrix_float4x4 modelMatrix;
    matrix_float4x4 normalMatrix;
    vector_float4 baseColorFactor;
    vector_float4 emissiveFactor;
    vector_float4 surfaceFactors;
    vector_float4 nodeGraphFactors;
};

struct MetalMaterialTextureSlots
{
    uint32_t baseColor = 0;
    uint32_t normal = 0;
    uint32_t metallic = 0;
    uint32_t roughness = 0;
    uint32_t occlusion = 0;
    uint32_t emissive = 0;
    uint32_t secondaryBaseColor = 0;
    uint32_t secondaryNormal = 0;
    uint32_t secondaryMetallic = 0;
    uint32_t secondaryRoughness = 0;
    uint32_t secondaryOcclusion = 0;
    uint32_t secondaryEmissive = 0;
    uint32_t blendMask = 0;
};

struct MetalRenderSubmesh
{
    entt::entity entity = entt::null;
    id<MTLBuffer> vertexBuffer = nil;
    id<MTLBuffer> indexBuffer = nil;
    NSUInteger indexCount = 0;
    uint32_t materialBindingIndex = 0;
    MaterialPushConstants material;
};

TextureData CreateSolidTexture(std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha)
{
    TextureData texture{};
    texture.width = 1;
    texture.height = 1;
    texture.channelCount = 4;
    texture.pixels = { red, green, blue, alpha };
    return texture;
}

TextureData CreateFlatNormalTexture()
{
    return CreateSolidTexture(128, 128, 255, 255);
}

std::string BuildTextureCacheKey(const std::string& path, MetalTextureFormat textureFormat)
{
    return path + (textureFormat == MetalTextureFormat::SrgbColor ? "|srgb" : "|linear");
}

std::string BuildImGuiIniPath()
{
    return (std::filesystem::path(MINIENGINE_PROJECT_DIR) / "imgui.ini").string();
}

std::filesystem::path FindPreferredUiFontPath()
{
    constexpr std::array<const char*, 4> kCandidates = {
        "/System/Library/Fonts/Supplemental/SFNS.ttf",
        "/System/Library/Fonts/Supplemental/Helvetica.ttc",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Supplemental/Verdana.ttf"
    };

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
    const ImVec4 baseBg = ImVec4(0.141f, 0.141f, 0.141f, 1.0f);
    const ImVec4 elevatedBg = ImVec4(0.165f, 0.165f, 0.165f, 1.0f);
    const ImVec4 activeBg = ImVec4(0.188f, 0.188f, 0.188f, 1.0f);
    const ImVec4 hoverBg = ImVec4(0.212f, 0.212f, 0.212f, 1.0f);
    const ImVec4 strongBg = ImVec4(0.251f, 0.251f, 0.251f, 1.0f);
    const ImVec4 border = ImVec4(0.314f, 0.314f, 0.314f, 1.0f);
    const ImVec4 accent = ImVec4(0.380f, 0.380f, 0.380f, 1.0f);
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

RenderExtent ClampExtent(RenderExtent extent)
{
    return RenderExtent{
        std::max(extent.width, 1u),
        std::max(extent.height, 1u)
    };
}

matrix_float4x4 ToMetalMatrix(const glm::mat4& matrix)
{
    matrix_float4x4 result;
    result.columns[0] = { matrix[0][0], matrix[0][1], matrix[0][2], matrix[0][3] };
    result.columns[1] = { matrix[1][0], matrix[1][1], matrix[1][2], matrix[1][3] };
    result.columns[2] = { matrix[2][0], matrix[2][1], matrix[2][2], matrix[2][3] };
    result.columns[3] = { matrix[3][0], matrix[3][1], matrix[3][2], matrix[3][3] };
    return result;
}

vector_float4 ToMetalVector4(const float value[4])
{
    return { value[0], value[1], value[2], value[3] };
}

id<MTLTexture> CreateTexture(
    id<MTLDevice> device,
    const TextureData& textureData,
    MetalTextureFormat textureFormat
)
{
    if (device == nil)
    {
        throw std::runtime_error("Cannot create Metal texture without a device");
    }
    if (!textureData.IsValid())
    {
        throw std::runtime_error("Cannot create Metal texture from invalid pixel data");
    }

    const MTLPixelFormat pixelFormat =
        textureFormat == MetalTextureFormat::SrgbColor
        ? MTLPixelFormatRGBA8Unorm_sRGB
        : MTLPixelFormatRGBA8Unorm;
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixelFormat
                                                           width:static_cast<NSUInteger>(textureData.width)
                                                          height:static_cast<NSUInteger>(textureData.height)
                                                       mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;

    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    if (texture == nil)
    {
        throw std::runtime_error("Failed to create Metal texture resource");
    }

    const MTLRegion region = MTLRegionMake2D(
        0,
        0,
        static_cast<NSUInteger>(textureData.width),
        static_cast<NSUInteger>(textureData.height)
    );
    [texture replaceRegion:region
               mipmapLevel:0
                 withBytes:textureData.pixels.data()
               bytesPerRow:static_cast<NSUInteger>(textureData.width) * 4];
    return texture;
}

MetalSceneFrameUniforms BuildSceneFrameUniforms(const ViewportMatrices& matrices, const glm::vec3& cameraPosition)
{
    MetalSceneFrameUniforms uniforms{};
    uniforms.viewProjectionMatrix = ToMetalMatrix(matrices.renderProjection * matrices.view);

    const glm::vec3 lightDirection = glm::normalize(glm::vec3(-0.6f, -1.0f, -0.35f));
    uniforms.cameraWorldPosition = { cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f };
    uniforms.lightDirectionAndIntensity = { lightDirection.x, lightDirection.y, lightDirection.z, 2.25f };
    uniforms.lightColorAndAmbient = { 1.0f, 0.98f, 0.95f, 0.2f };
    return uniforms;
}

MetalSceneDrawUniforms BuildSceneDrawUniforms(const glm::mat4& modelMatrix, const MaterialPushConstants& material)
{
    MetalSceneDrawUniforms uniforms{};
    uniforms.modelMatrix = ToMetalMatrix(modelMatrix);
    uniforms.normalMatrix = ToMetalMatrix(glm::transpose(glm::inverse(modelMatrix)));
    uniforms.baseColorFactor = ToMetalVector4(material.baseColorFactor);
    uniforms.emissiveFactor = ToMetalVector4(material.emissiveFactor);
    uniforms.surfaceFactors = ToMetalVector4(material.surfaceFactors);
    uniforms.nodeGraphFactors = ToMetalVector4(material.nodeGraphFactors);
    return uniforms;
}

ImTextureID ToImGuiTextureId(id<MTLTexture> texture)
{
    return texture != nil ? (ImTextureID)(__bridge void*)texture : ImTextureID{};
}
}

struct MetalRenderer::Impl
{
    SDL_MetalView metalView = nullptr;
    CAMetalLayer* metalLayer = nil;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> commandQueue = nil;
    id<MTLRenderPipelineState> scenePipelineState = nil;
    id<MTLDepthStencilState> sceneDepthStencilState = nil;
    id<MTLTexture> sceneColorTexture = nil;
    id<MTLTexture> sceneDepthTexture = nil;
    RenderExtent sceneExtent{};
    std::vector<id<MTLTexture>> textures;
    std::vector<MetalMaterialTextureSlots> materialTextureSlots;
    std::vector<MetalRenderSubmesh> renderSubmeshes;
    std::string iniFilePath;
};

MetalRenderer::MetalRenderer(
    Window& window,
    std::shared_ptr<RendererSharedState> sharedState,
    std::optional<std::string> startupModelPath
)
    : EditorRenderBackendBase(window, std::move(sharedState), RenderBackendType::Metal, std::move(startupModelPath)),
      m_impl(std::make_unique<Impl>())
{
    m_impl->iniFilePath = BuildImGuiIniPath();

    @autoreleasepool
    {
        m_impl->device = MTLCreateSystemDefaultDevice();
        if (m_impl->device == nil)
        {
            throw std::runtime_error("Failed to create Metal device");
        }

        m_impl->commandQueue = [m_impl->device newCommandQueue];
        if (m_impl->commandQueue == nil)
        {
            throw std::runtime_error("Failed to create Metal command queue");
        }

        m_impl->metalView = SDL_Metal_CreateView(GetWindow().GetSDLWindow());
        if (m_impl->metalView == nullptr)
        {
            throw std::runtime_error(std::string("SDL_Metal_CreateView failed: ") + SDL_GetError());
        }

        m_impl->metalLayer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(m_impl->metalView);
        if (m_impl->metalLayer == nil)
        {
            throw std::runtime_error("SDL_Metal_GetLayer returned null");
        }

        m_impl->metalLayer.device = m_impl->device;
        m_impl->metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        m_impl->metalLayer.framebufferOnly = NO;
        m_impl->metalLayer.opaque = YES;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = m_impl->iniFilePath.c_str();
    ConfigureImGuiStyle();
    ConfigureImGuiFonts(io);

    if (!ImGui_ImplSDL3_InitForMetal(GetWindow().GetSDLWindow()))
    {
        throw std::runtime_error("Failed to initialize ImGui SDL3 Metal backend");
    }

    if (!ImGui_ImplMetal_Init(m_impl->device))
    {
        throw std::runtime_error("Failed to initialize ImGui Metal renderer backend");
    }

    CreateScenePipeline();
    UploadSceneResources();

    LOG_INFO(
        "Metal renderer initialized. Editor UI and scene viewport rendering are active."
    );
}

MetalRenderer::~MetalRenderer()
{
    if (ImGui::GetCurrentContext() != nullptr)
    {
        ImGui::DestroyPlatformWindows();
        ImGui_ImplMetal_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }

    if (m_impl && m_impl->metalView != nullptr)
    {
        SDL_Metal_DestroyView(m_impl->metalView);
        m_impl->metalView = nullptr;
        m_impl->metalLayer = nil;
        m_impl->commandQueue = nil;
        m_impl->device = nil;
    }
}

void MetalRenderer::DrawFrame()
{
    if (!TickSharedFrame())
    {
        return;
    }

    if (ProcessPendingOperations())
    {
        UploadSceneResources();
    }

    @autoreleasepool
    {
        int width = 0;
        int height = 0;
        if (!SDL_GetWindowSizeInPixels(GetWindow().GetSDLWindow(), &width, &height))
        {
            throw std::runtime_error(std::string("SDL_GetWindowSizeInPixels failed: ") + SDL_GetError());
        }

        if (width <= 0 || height <= 0)
        {
            return;
        }

        const RenderExtent windowExtent{
            static_cast<uint32_t>(width),
            static_cast<uint32_t>(height)
        };
        if (!State().requestedViewportExtent.IsValid())
        {
            State().requestedViewportExtent = windowExtent;
        }
        EnsureSceneViewportResources(State().requestedViewportExtent);
        UpdateViewportMatrices(m_impl->sceneExtent.IsValid() ? m_impl->sceneExtent : windowExtent);

        const float displayScale = SDL_GetWindowDisplayScale(GetWindow().GetSDLWindow());
        m_impl->metalLayer.contentsScale = displayScale > 0.0f ? static_cast<CGFloat>(displayScale) : 1.0;
        m_impl->metalLayer.drawableSize = CGSizeMake(static_cast<CGFloat>(width), static_cast<CGFloat>(height));

        id<CAMetalDrawable> drawable = [m_impl->metalLayer nextDrawable];
        if (drawable == nil)
        {
            return;
        }

        MTLRenderPassDescriptor* renderPassDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
        renderPassDescriptor.colorAttachments[0].texture = drawable.texture;
        renderPassDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
        renderPassDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
        renderPassDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(0.094, 0.110, 0.133, 1.0);

        ImGui_ImplMetal_NewFrame(renderPassDescriptor);
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        State().editorUi.BeginFrame(GetWindow().GetSDLWindow(), State().engineSettings);
        const EditorUiFrameResult uiFrame = DrawEditorUi(
            GetViewportTextureId(),
            m_impl->sceneExtent.IsValid() ? m_impl->sceneExtent : windowExtent
        );
        ApplyUiActions(uiFrame);
        if (State().renderablesDirty)
        {
            UploadSceneResources();
            State().renderablesDirty = false;
        }
        ImGui::Render();

        id<MTLCommandBuffer> commandBuffer = [m_impl->commandQueue commandBuffer];
        if (commandBuffer == nil)
        {
            throw std::runtime_error("Failed to create Metal command buffer");
        }

        if (m_impl->sceneColorTexture != nil && m_impl->sceneDepthTexture != nil)
        {
            MTLRenderPassDescriptor* scenePassDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
            scenePassDescriptor.colorAttachments[0].texture = m_impl->sceneColorTexture;
            scenePassDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
            scenePassDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
            scenePassDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(0.08, 0.10, 0.16, 1.0);
            scenePassDescriptor.depthAttachment.texture = m_impl->sceneDepthTexture;
            scenePassDescriptor.depthAttachment.loadAction = MTLLoadActionClear;
            scenePassDescriptor.depthAttachment.storeAction = MTLStoreActionDontCare;
            scenePassDescriptor.depthAttachment.clearDepth = 1.0;

            id<MTLRenderCommandEncoder> sceneEncoder =
                [commandBuffer renderCommandEncoderWithDescriptor:scenePassDescriptor];
            if (sceneEncoder == nil)
            {
                throw std::runtime_error("Failed to create Metal scene render command encoder");
            }

            [sceneEncoder setRenderPipelineState:m_impl->scenePipelineState];
            [sceneEncoder setDepthStencilState:m_impl->sceneDepthStencilState];
            [sceneEncoder setCullMode:MTLCullModeNone];
            [sceneEncoder setFrontFacingWinding:MTLWindingCounterClockwise];
            [sceneEncoder setViewport:MTLViewport{
                0.0,
                0.0,
                static_cast<double>(m_impl->sceneExtent.width),
                static_cast<double>(m_impl->sceneExtent.height),
                0.0,
                1.0
            }];

            const MetalSceneFrameUniforms frameUniforms =
                BuildSceneFrameUniforms(State().viewportMatrices, State().camera.position);
            [sceneEncoder setVertexBytes:&frameUniforms length:sizeof(frameUniforms) atIndex:1];
            [sceneEncoder setFragmentBytes:&frameUniforms length:sizeof(frameUniforms) atIndex:0];

            for (const MetalRenderSubmesh& renderSubmesh : m_impl->renderSubmeshes)
            {
                if (renderSubmesh.vertexBuffer == nil || renderSubmesh.indexBuffer == nil || renderSubmesh.indexCount == 0)
                {
                    continue;
                }

                const glm::mat4 modelMatrix = RenderWorld().GetModelMatrix(renderSubmesh.entity);
                const MetalSceneDrawUniforms drawUniforms = BuildSceneDrawUniforms(modelMatrix, renderSubmesh.material);
                const MetalMaterialTextureSlots& slots = m_impl->materialTextureSlots.at(renderSubmesh.materialBindingIndex);
                [sceneEncoder setVertexBuffer:renderSubmesh.vertexBuffer offset:0 atIndex:0];
                [sceneEncoder setVertexBytes:&drawUniforms length:sizeof(drawUniforms) atIndex:2];
                [sceneEncoder setFragmentBytes:&drawUniforms length:sizeof(drawUniforms) atIndex:1];
                [sceneEncoder setFragmentTexture:m_impl->textures.at(slots.baseColor) atIndex:0];
                [sceneEncoder setFragmentTexture:m_impl->textures.at(slots.normal) atIndex:1];
                [sceneEncoder setFragmentTexture:m_impl->textures.at(slots.metallic) atIndex:2];
                [sceneEncoder setFragmentTexture:m_impl->textures.at(slots.roughness) atIndex:3];
                [sceneEncoder setFragmentTexture:m_impl->textures.at(slots.occlusion) atIndex:4];
                [sceneEncoder setFragmentTexture:m_impl->textures.at(slots.emissive) atIndex:5];
                [sceneEncoder setFragmentTexture:m_impl->textures.at(slots.secondaryBaseColor) atIndex:6];
                [sceneEncoder setFragmentTexture:m_impl->textures.at(slots.secondaryNormal) atIndex:7];
                [sceneEncoder setFragmentTexture:m_impl->textures.at(slots.secondaryMetallic) atIndex:8];
                [sceneEncoder setFragmentTexture:m_impl->textures.at(slots.secondaryRoughness) atIndex:9];
                [sceneEncoder setFragmentTexture:m_impl->textures.at(slots.secondaryOcclusion) atIndex:10];
                [sceneEncoder setFragmentTexture:m_impl->textures.at(slots.secondaryEmissive) atIndex:11];
                [sceneEncoder setFragmentTexture:m_impl->textures.at(slots.blendMask) atIndex:12];
                [sceneEncoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                        indexCount:renderSubmesh.indexCount
                                         indexType:MTLIndexTypeUInt32
                                       indexBuffer:renderSubmesh.indexBuffer
                                 indexBufferOffset:0];
            }

            [sceneEncoder endEncoding];
        }

        id<MTLRenderCommandEncoder> commandEncoder =
            [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];
        if (commandEncoder == nil)
        {
            throw std::runtime_error("Failed to create Metal render command encoder");
        }

        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commandBuffer, commandEncoder);
        [commandEncoder endEncoding];
        [commandBuffer presentDrawable:drawable];
        [commandBuffer commit];
    }
}

void MetalRenderer::HandleBackendEvent(const SDL_Event& event)
{
    if (ImGui::GetCurrentContext() == nullptr)
    {
        return;
    }

    ImGui_ImplSDL3_ProcessEvent(&event);
}

bool MetalRenderer::WantsKeyboardCapture() const
{
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureKeyboard;
}

void MetalRenderer::CreateScenePipeline()
{
    @autoreleasepool
    {
        NSString* shaderSource = [NSString stringWithUTF8String:kSceneShaderSource];
        NSError* libraryError = nil;
        id<MTLLibrary> library = [m_impl->device newLibraryWithSource:shaderSource options:nil error:&libraryError];
        if (library == nil)
        {
            const std::string message =
                libraryError != nil && libraryError.localizedDescription != nil
                ? std::string([[libraryError localizedDescription] UTF8String])
                : std::string("Unknown Metal shader compilation error");
            throw std::runtime_error("Failed to compile Metal scene shaders: " + message);
        }

        id<MTLFunction> vertexFunction = [library newFunctionWithName:@"scene_vertex"];
        id<MTLFunction> fragmentFunction = [library newFunctionWithName:@"scene_fragment"];
        if (vertexFunction == nil || fragmentFunction == nil)
        {
            throw std::runtime_error("Failed to resolve Metal scene shader entry points");
        }

        MTLVertexDescriptor* vertexDescriptor = [MTLVertexDescriptor vertexDescriptor];
        vertexDescriptor.attributes[0].format = MTLVertexFormatFloat3;
        vertexDescriptor.attributes[0].offset = static_cast<NSUInteger>(offsetof(Vertex, position));
        vertexDescriptor.attributes[0].bufferIndex = 0;
        vertexDescriptor.attributes[1].format = MTLVertexFormatFloat3;
        vertexDescriptor.attributes[1].offset = static_cast<NSUInteger>(offsetof(Vertex, color));
        vertexDescriptor.attributes[1].bufferIndex = 0;
        vertexDescriptor.attributes[2].format = MTLVertexFormatFloat2;
        vertexDescriptor.attributes[2].offset = static_cast<NSUInteger>(offsetof(Vertex, texCoord));
        vertexDescriptor.attributes[2].bufferIndex = 0;
        vertexDescriptor.attributes[3].format = MTLVertexFormatFloat3;
        vertexDescriptor.attributes[3].offset = static_cast<NSUInteger>(offsetof(Vertex, normal));
        vertexDescriptor.attributes[3].bufferIndex = 0;
        vertexDescriptor.attributes[4].format = MTLVertexFormatFloat4;
        vertexDescriptor.attributes[4].offset = static_cast<NSUInteger>(offsetof(Vertex, tangent));
        vertexDescriptor.attributes[4].bufferIndex = 0;
        vertexDescriptor.layouts[0].stride = static_cast<NSUInteger>(sizeof(Vertex));
        vertexDescriptor.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

        MTLRenderPipelineDescriptor* pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
        pipelineDescriptor.vertexFunction = vertexFunction;
        pipelineDescriptor.fragmentFunction = fragmentFunction;
        pipelineDescriptor.vertexDescriptor = vertexDescriptor;
        pipelineDescriptor.colorAttachments[0].pixelFormat = kSceneColorFormat;
        pipelineDescriptor.depthAttachmentPixelFormat = kSceneDepthFormat;
        pipelineDescriptor.colorAttachments[0].blendingEnabled = YES;
        pipelineDescriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        pipelineDescriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        pipelineDescriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        pipelineDescriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        pipelineDescriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        pipelineDescriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

        NSError* pipelineError = nil;
        m_impl->scenePipelineState =
            [m_impl->device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&pipelineError];
        if (m_impl->scenePipelineState == nil)
        {
            const std::string message =
                pipelineError != nil && pipelineError.localizedDescription != nil
                ? std::string([[pipelineError localizedDescription] UTF8String])
                : std::string("Unknown Metal pipeline creation error");
            throw std::runtime_error("Failed to create Metal scene pipeline: " + message);
        }

        MTLDepthStencilDescriptor* depthDescriptor = [[MTLDepthStencilDescriptor alloc] init];
        depthDescriptor.depthCompareFunction = MTLCompareFunctionLess;
        depthDescriptor.depthWriteEnabled = YES;
        m_impl->sceneDepthStencilState = [m_impl->device newDepthStencilStateWithDescriptor:depthDescriptor];
        if (m_impl->sceneDepthStencilState == nil)
        {
            throw std::runtime_error("Failed to create Metal scene depth stencil state");
        }
    }
}

void MetalRenderer::EnsureSceneViewportResources(RenderExtent extent)
{
    const RenderExtent clampedExtent = ClampExtent(extent);
    if (m_impl->sceneColorTexture != nil &&
        m_impl->sceneDepthTexture != nil &&
        m_impl->sceneExtent.width == clampedExtent.width &&
        m_impl->sceneExtent.height == clampedExtent.height)
    {
        return;
    }

    @autoreleasepool
    {
        MTLTextureDescriptor* colorDescriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kSceneColorFormat
                                                               width:clampedExtent.width
                                                              height:clampedExtent.height
                                                           mipmapped:NO];
        colorDescriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        colorDescriptor.storageMode = MTLStorageModePrivate;
        m_impl->sceneColorTexture = [m_impl->device newTextureWithDescriptor:colorDescriptor];
        if (m_impl->sceneColorTexture == nil)
        {
            throw std::runtime_error("Failed to create Metal viewport color texture");
        }

        MTLTextureDescriptor* depthDescriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kSceneDepthFormat
                                                               width:clampedExtent.width
                                                              height:clampedExtent.height
                                                           mipmapped:NO];
        depthDescriptor.usage = MTLTextureUsageRenderTarget;
        depthDescriptor.storageMode = MTLStorageModePrivate;
        m_impl->sceneDepthTexture = [m_impl->device newTextureWithDescriptor:depthDescriptor];
        if (m_impl->sceneDepthTexture == nil)
        {
            throw std::runtime_error("Failed to create Metal viewport depth texture");
        }

        m_impl->sceneExtent = clampedExtent;
        LOG_INFO(
            "Metal viewport resources created: {}x{}",
            m_impl->sceneExtent.width,
            m_impl->sceneExtent.height
        );
    }
}

void MetalRenderer::UploadSceneResources()
{
    @autoreleasepool
    {
        std::vector<id<MTLTexture>> newTextures;
        std::vector<MetalMaterialTextureSlots> newMaterialTextureSlots;
        std::vector<MetalRenderSubmesh> newRenderSubmeshes;
        std::unordered_map<std::string, uint32_t> textureCache;

        auto getOrCreateTexture = [&](const std::string& cacheKey, const TextureData& textureData, MetalTextureFormat textureFormat) -> uint32_t
        {
            if (const auto iterator = textureCache.find(cacheKey); iterator != textureCache.end())
            {
                return iterator->second;
            }

            const uint32_t textureIndex = static_cast<uint32_t>(newTextures.size());
            newTextures.push_back(CreateTexture(m_impl->device, textureData, textureFormat));
            textureCache.emplace(cacheKey, textureIndex);
            return textureIndex;
        };

        const uint32_t defaultBaseColorIndex = getOrCreateTexture(
            "__default_base_color__",
            CreateSolidTexture(255, 255, 255, 255),
            MetalTextureFormat::SrgbColor
        );
        const uint32_t defaultNormalIndex = getOrCreateTexture(
            "__default_normal__",
            CreateFlatNormalTexture(),
            MetalTextureFormat::LinearData
        );
        const uint32_t defaultMetallicIndex = getOrCreateTexture(
            "__default_metallic__",
            CreateSolidTexture(255, 255, 255, 255),
            MetalTextureFormat::LinearData
        );
        const uint32_t defaultRoughnessIndex = getOrCreateTexture(
            "__default_roughness__",
            CreateSolidTexture(255, 255, 255, 255),
            MetalTextureFormat::LinearData
        );
        const uint32_t defaultOcclusionIndex = getOrCreateTexture(
            "__default_occlusion__",
            CreateSolidTexture(255, 255, 255, 255),
            MetalTextureFormat::LinearData
        );
        const uint32_t defaultEmissiveIndex = getOrCreateTexture(
            "__default_emissive__",
            CreateSolidTexture(255, 255, 255, 255),
            MetalTextureFormat::SrgbColor
        );
        const uint32_t defaultBlendMaskIndex = getOrCreateTexture(
            "__default_blend_mask__",
            CreateSolidTexture(255, 255, 255, 255),
            MetalTextureFormat::LinearData
        );

        const uint32_t defaultMaterialBindingIndex = static_cast<uint32_t>(newMaterialTextureSlots.size());
        newMaterialTextureSlots.push_back(MetalMaterialTextureSlots{
            defaultBaseColorIndex,
            defaultNormalIndex,
            defaultMetallicIndex,
            defaultRoughnessIndex,
            defaultOcclusionIndex,
            defaultEmissiveIndex,
            defaultBaseColorIndex,
            defaultNormalIndex,
            defaultMetallicIndex,
            defaultRoughnessIndex,
            defaultOcclusionIndex,
            defaultEmissiveIndex,
            defaultBlendMaskIndex
        });

        auto loadTextureIndex = [&](const std::string& texturePath, MetalTextureFormat textureFormat, uint32_t fallbackIndex) -> uint32_t
        {
            if (texturePath.empty())
            {
                return fallbackIndex;
            }

            const std::string cacheKey = BuildTextureCacheKey(texturePath, textureFormat);
            if (const auto iterator = textureCache.find(cacheKey); iterator != textureCache.end())
            {
                return iterator->second;
            }

            try
            {
                const uint32_t textureIndex = static_cast<uint32_t>(newTextures.size());
                newTextures.push_back(CreateTexture(
                    m_impl->device,
                    TextureLoader::LoadRGBA8(texturePath),
                    textureFormat
                ));
                textureCache.emplace(cacheKey, textureIndex);
                return textureIndex;
            }
            catch (const std::exception& error)
            {
                LOG_ERROR("Failed to load Metal model texture '{}': {}", texturePath, error.what());
                return fallbackIndex;
            }
        };

        newRenderSubmeshes.reserve(RenderWorld().GetRenderSubmeshes().size());

        for (const CpuRenderSubmesh& cpuRenderSubmesh : RenderWorld().GetRenderSubmeshes())
        {
            if (!cpuRenderSubmesh.mesh.IsValid())
            {
                continue;
            }

            const NSUInteger vertexBufferSize =
                static_cast<NSUInteger>(sizeof(Vertex) * cpuRenderSubmesh.mesh.vertices.size());
            const NSUInteger indexBufferSize =
                static_cast<NSUInteger>(sizeof(uint32_t) * cpuRenderSubmesh.mesh.indices.size());

            MetalRenderSubmesh renderSubmesh{};
            renderSubmesh.entity = cpuRenderSubmesh.entity;
            renderSubmesh.material = cpuRenderSubmesh.material;
            renderSubmesh.indexCount = static_cast<NSUInteger>(cpuRenderSubmesh.mesh.indices.size());
            renderSubmesh.vertexBuffer = [m_impl->device newBufferWithBytes:cpuRenderSubmesh.mesh.vertices.data()
                                                                     length:vertexBufferSize
                                                                    options:MTLResourceStorageModeShared];
            renderSubmesh.indexBuffer = [m_impl->device newBufferWithBytes:cpuRenderSubmesh.mesh.indices.data()
                                                                    length:indexBufferSize
                                                                   options:MTLResourceStorageModeShared];
            if (renderSubmesh.vertexBuffer == nil || renderSubmesh.indexBuffer == nil)
            {
                throw std::runtime_error("Failed to upload Metal scene mesh buffers");
            }

            if (!cpuRenderSubmesh.hasTexCoords)
            {
                renderSubmesh.materialBindingIndex = defaultMaterialBindingIndex;
                newRenderSubmeshes.push_back(std::move(renderSubmesh));
                continue;
            }

            MetalMaterialTextureSlots slots = newMaterialTextureSlots[defaultMaterialBindingIndex];
            slots.baseColor = loadTextureIndex(
                cpuRenderSubmesh.textures.baseColor,
                MetalTextureFormat::SrgbColor,
                defaultBaseColorIndex
            );
            slots.normal = loadTextureIndex(
                cpuRenderSubmesh.textures.normal,
                MetalTextureFormat::LinearData,
                defaultNormalIndex
            );
            slots.metallic = loadTextureIndex(
                cpuRenderSubmesh.textures.metallic,
                MetalTextureFormat::LinearData,
                defaultMetallicIndex
            );
            slots.roughness = loadTextureIndex(
                cpuRenderSubmesh.textures.roughness,
                MetalTextureFormat::LinearData,
                defaultRoughnessIndex
            );
            slots.occlusion = loadTextureIndex(
                cpuRenderSubmesh.textures.occlusion,
                MetalTextureFormat::LinearData,
                defaultOcclusionIndex
            );
            slots.emissive = loadTextureIndex(
                cpuRenderSubmesh.textures.emissive,
                MetalTextureFormat::SrgbColor,
                defaultEmissiveIndex
            );
            slots.secondaryBaseColor = loadTextureIndex(
                cpuRenderSubmesh.textures.secondaryBaseColor,
                MetalTextureFormat::SrgbColor,
                slots.baseColor
            );
            slots.secondaryNormal = loadTextureIndex(
                cpuRenderSubmesh.textures.secondaryNormal,
                MetalTextureFormat::LinearData,
                slots.normal
            );
            slots.secondaryMetallic = loadTextureIndex(
                cpuRenderSubmesh.textures.secondaryMetallic,
                MetalTextureFormat::LinearData,
                slots.metallic
            );
            slots.secondaryRoughness = loadTextureIndex(
                cpuRenderSubmesh.textures.secondaryRoughness,
                MetalTextureFormat::LinearData,
                slots.roughness
            );
            slots.secondaryOcclusion = loadTextureIndex(
                cpuRenderSubmesh.textures.secondaryOcclusion,
                MetalTextureFormat::LinearData,
                slots.occlusion
            );
            slots.secondaryEmissive = loadTextureIndex(
                cpuRenderSubmesh.textures.secondaryEmissive,
                MetalTextureFormat::SrgbColor,
                slots.emissive
            );
            slots.blendMask = loadTextureIndex(
                cpuRenderSubmesh.textures.blendMask,
                MetalTextureFormat::LinearData,
                defaultBlendMaskIndex
            );

            renderSubmesh.materialBindingIndex = static_cast<uint32_t>(newMaterialTextureSlots.size());
            newMaterialTextureSlots.push_back(slots);
            newRenderSubmeshes.push_back(std::move(renderSubmesh));
        }

        m_impl->textures = std::move(newTextures);
        m_impl->materialTextureSlots = std::move(newMaterialTextureSlots);
        m_impl->renderSubmeshes = std::move(newRenderSubmeshes);
        LOG_INFO(
            "Metal scene resources uploaded: {} submeshes, {} textures, {} material bindings",
            m_impl->renderSubmeshes.size(),
            m_impl->textures.size(),
            m_impl->materialTextureSlots.size()
        );
    }
}

ImTextureID MetalRenderer::GetViewportTextureId() const
{
    return ToImGuiTextureId(m_impl->sceneColorTexture);
}
