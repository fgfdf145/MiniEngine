#include "editor_ui_internal.h"

#include <engine/asset/model_loader.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace me
{

namespace
{
void DrawTexturePathRow(const char* label, const std::string& path)
{
    if (path.empty())
    {
        ImGui::TextDisabled("%s: <none>", label);
        return;
    }

    std::error_code errorCode;
    const bool exists = std::filesystem::exists(path, errorCode);
    const ImVec4 statusColor = exists ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f) : ImVec4(0.95f, 0.4f, 0.4f, 1.0f);
    ImGui::TextWrapped("%s: %s", label, path.c_str());
    ImGui::SameLine();
    ImGui::TextColored(statusColor, "[%s]", exists ? "resolved" : "missing");
}

size_t CountMaterialGraphSecondaryTextures(const MaterialTextureBlendGraph& blendGraph);

struct MaterialTexturePathRow
{
    const char* label = "";
    const std::string ModelImportedMaterialInfo::* path = nullptr;
};

struct BlendGraphTexturePathRow
{
    const char* label = "";
    const std::string MaterialTextureBlendGraph::* path = nullptr;
};

constexpr std::array<MaterialTexturePathRow, 14> kPrimaryMaterialTextureRows = {{{"Base Color", &ModelImportedMaterialInfo::baseColorTexturePath},
                                                                                 {"Normal", &ModelImportedMaterialInfo::normalTexturePath},
                                                                                 {"Metallic", &ModelImportedMaterialInfo::metallicTexturePath},
                                                                                 {"Roughness", &ModelImportedMaterialInfo::roughnessTexturePath},
                                                                                 {"Occlusion", &ModelImportedMaterialInfo::occlusionTexturePath},
                                                                                 {"Emissive", &ModelImportedMaterialInfo::emissiveTexturePath},
                                                                                 {"Clearcoat", &ModelImportedMaterialInfo::clearcoatTexturePath},
                                                                                 {"Clearcoat Roughness", &ModelImportedMaterialInfo::clearcoatRoughnessTexturePath},
                                                                                 {"Sheen Color", &ModelImportedMaterialInfo::sheenColorTexturePath},
                                                                                 {"Sheen Roughness", &ModelImportedMaterialInfo::sheenRoughnessTexturePath},
                                                                                 {"Anisotropy", &ModelImportedMaterialInfo::anisotropyTexturePath},
                                                                                 {"Specular", &ModelImportedMaterialInfo::specularTexturePath},
                                                                                 {"Specular Color", &ModelImportedMaterialInfo::specularColorTexturePath},
                                                                                 {"Clearcoat Normal", &ModelImportedMaterialInfo::clearcoatNormalTexturePath}}};

constexpr std::array<BlendGraphTexturePathRow, 7> kSecondaryMaterialTextureRows = {{{"Blend Mask", &MaterialTextureBlendGraph::blendMaskTexturePath},
                                                                                    {"Layer B Base", &MaterialTextureBlendGraph::secondaryBaseColorTexturePath},
                                                                                    {"Layer B Normal", &MaterialTextureBlendGraph::secondaryNormalTexturePath},
                                                                                    {"Layer B Metallic", &MaterialTextureBlendGraph::secondaryMetallicTexturePath},
                                                                                    {"Layer B Roughness", &MaterialTextureBlendGraph::secondaryRoughnessTexturePath},
                                                                                    {"Layer B Occlusion", &MaterialTextureBlendGraph::secondaryOcclusionTexturePath},
                                                                                    {"Layer B Emissive", &MaterialTextureBlendGraph::secondaryEmissiveTexturePath}}};

template <typename TObject, typename TEntry, size_t TSize>
void DrawTexturePathRows(const TObject& object, const std::array<TEntry, TSize>& rows)
{
    for (const TEntry& row : rows)
    {
        DrawTexturePathRow(row.label, object.*(row.path));
    }
}
}

bool HasSecondaryMaterialLayer(const MaterialTextureBlendGraph& blendGraph)
{
    return blendGraph.enabled || CountMaterialGraphSecondaryTextures(blendGraph) > 0;
}

void DrawPrimaryMaterialTextureRows(const ModelImportedMaterialInfo& material)
{
    DrawTexturePathRows(material, kPrimaryMaterialTextureRows);
}

void DrawSecondaryMaterialTextureRows(const MaterialTextureBlendGraph& blendGraph)
{
    DrawTexturePathRows(blendGraph, kSecondaryMaterialTextureRows);
}

namespace
{
size_t CountMaterialGraphSecondaryTextures(const MaterialTextureBlendGraph& blendGraph)
{
    const std::array<const std::string*, 7> paths = {
        &blendGraph.secondaryBaseColorTexturePath,
        &blendGraph.secondaryNormalTexturePath,
        &blendGraph.secondaryMetallicTexturePath,
        &blendGraph.secondaryRoughnessTexturePath,
        &blendGraph.secondaryOcclusionTexturePath,
        &blendGraph.secondaryEmissiveTexturePath,
        &blendGraph.blendMaskTexturePath};
    return static_cast<size_t>(std::count_if(paths.begin(), paths.end(), [](const std::string* value)
                                             {
                                                 return value != nullptr && !value->empty();
                                             }));
}

// A typed or pasted path: surrounding blanks and quotes dropped, and "~" expanded where there is a
// home directory, since a shell would have done that.
std::string CleanTypedPath(std::string path)
{
    const size_t first = path.find_first_not_of(" \t\r\n\"'");
    const size_t last = path.find_last_not_of(" \t\r\n\"'");
    path = first == std::string::npos ? std::string{} : path.substr(first, last - first + 1);
#ifndef _WIN32
    if (path == "~" || path.starts_with("~/"))
    {
        if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
        {
            path = std::string(home) + path.substr(1);
        }
    }
#endif
    return path;
}

// Why the typed path cannot be used, or "" when it can.
std::string CheckTypedPath(FileDialogType type, const std::string& path)
{
    if (path.empty())
    {
        return "Enter a path.";
    }
    std::error_code ec;
    if (type == FileDialogType::SaveScene)
    {
        const std::filesystem::path parent = std::filesystem::path(path).parent_path();
        if (!parent.empty() && !std::filesystem::is_directory(parent, ec))
        {
            return "The folder does not exist: " + parent.string();
        }
        return {};
    }
    if (!std::filesystem::is_regular_file(path, ec))
    {
        return "No file at that path.";
    }
    return {};
}

const char* FilePathPromptHint(FileDialogType type)
{
    switch (type)
    {
    case FileDialogType::OpenModel:
        return "Model file to import (.gltf or .glb):";
    case FileDialogType::OpenTexture:
        return "Texture file (.png, .jpg, .hdr, .exr, ...):";
    case FileDialogType::OpenScene:
        return "Scene file to load (.yaml):";
    case FileDialogType::SaveScene:
        return "Save the scene to (.yaml):";
    }
    return "File path:";
}
}

std::optional<std::string> PickFilePath(FileDialogType type, bool requested)
{
    constexpr const char* kTitle = "Enter File Path";
    // Only one modal is open at a time, so the prompts share one buffer.
    static std::string s_typedPath;
    static std::string s_typedPathError;

    if (requested)
    {
        if (SupportsNativeFileDialogs())
        {
            std::optional<std::string> chosen = ShowFileDialog(type);
            // Still supported afterwards: the user chose a file or cancelled.
            if (chosen.has_value() || SupportsNativeFileDialogs())
            {
                return chosen;
            }
        }
        s_typedPath.clear();
        s_typedPathError.clear();
        ImGui::OpenPopup(kTitle);
    }

    std::optional<std::string> chosen;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        const float uiScale = ImGui::GetStyle().FontScaleMain;
        ImGui::TextUnformatted(FilePathPromptHint(type));
        ImGui::TextDisabled("No file dialog is available here; type or paste the full path.");
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(520.0f * uiScale);
        const bool entered = ImGui::InputText("##typed_path", &s_typedPath, ImGuiInputTextFlags_EnterReturnsTrue);
        if (!s_typedPathError.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", s_typedPathError.c_str());
        }
        ImGui::Separator();

        if (ImGui::Button("OK", ImVec2(120.0f * uiScale, 0.0f)) || entered)
        {
            std::string path = CleanTypedPath(s_typedPath);
            if (type == FileDialogType::SaveScene && !path.empty() &&
                std::filesystem::path(path).extension().empty())
            {
                path += ".yaml"; // the native dialogs add it too
            }
            s_typedPathError = CheckTypedPath(type, path);
            if (s_typedPathError.empty())
            {
                chosen = std::move(path);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f * uiScale, 0.0f)))
        {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return chosen;
}

bool IsSupportedModelAssetPath(const std::filesystem::path& path)
{
    return ModelLoader::IsSupportedModelPath(path);
}

std::filesystem::path NormalizeFilesystemPath(const std::filesystem::path& path)
{
    std::error_code errorCode;
    const std::filesystem::path absolutePath = std::filesystem::absolute(path, errorCode);
    return errorCode ? path.lexically_normal() : absolutePath.lexically_normal();
}

const char* GetLightTypeLabel(LightType type)
{
    switch (type)
    {
    case LightType::Directional:
        return "Directional";
    case LightType::Point:
        return "Point";
    case LightType::Spot:
        return "Spot";
    case LightType::Area:
        return "Area";
    case LightType::Ambient:
        return "Ambient";
    default:
        return "Unknown";
    }
}

ImU32 GetLightTypeColor(LightType type)
{
    switch (type)
    {
    case LightType::Directional:
        return IM_COL32(255, 240, 128, 255);
    case LightType::Point:
        return IM_COL32(255, 196, 64, 255);
    case LightType::Spot:
        return IM_COL32(128, 220, 255, 255);
    case LightType::Area:
        return IM_COL32(180, 255, 160, 255);
    case LightType::Ambient:
        return IM_COL32(200, 180, 255, 255);
    default:
        return IM_COL32(220, 220, 220, 255);
    }
}

namespace
{
// Pixels of mouse travel that cross a field's whole range, about the width of a slider.
constexpr float kDragPixelsForFullRange = 300.0f;

float ResolveDragSpeed(float speed, float min, float max)
{
    return speed > 0.0f ? speed : (max - min) / kDragPixelsForFullRange;
}
}

bool DragFloatInRange(const char* label, float* value, float min, float max, const char* format, float speed)
{
    return ImGui::DragFloat(
        label,
        value,
        ResolveDragSpeed(speed, min, max),
        min,
        max,
        format,
        ImGuiSliderFlags_AlwaysClamp);
}

bool DragFloat3InRange(const char* label, float* values, float min, float max, const char* format, float speed)
{
    return ImGui::DragFloat3(
        label,
        values,
        ResolveDragSpeed(speed, min, max),
        min,
        max,
        format,
        ImGuiSliderFlags_AlwaysClamp);
}

bool DragIntInRange(const char* label, int* value, int min, int max)
{
    // Integer fields have few steps, so they get more travel per step than float fields.
    const float speed = std::max(static_cast<float>(max - min) / 150.0f, 0.02f);
    return ImGui::DragInt(label, value, speed, min, max, "%d", ImGuiSliderFlags_AlwaysClamp);
}
}
