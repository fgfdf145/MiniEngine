#include "material_editor_window.h"

#include <engine/asset/asset_paths.h>
#include <engine/asset/material_definition.h>
#include <engine/asset/material_graph_runtime.h>
#include <engine/asset/model_loader.h>
#include <engine/core/threading/task_system.h>
#include <engine/editor/editor_ui.h>
#include <engine/editor/services/scene_renderables.h>
#include <engine/editor/ui/editor_menu_toolbar.h>
#include <engine/editor/ui/editor_ui_internal.h>
#include <engine/editor/ui/framework/editor_context.h>
#include <engine/editor/ui/framework/editor_window_manager.h>
#include <engine/editor/ui_colors.h>
#include <engine/logic/editor_world.h>

#include <IconsPhosphor.h>
#include <TaskScheduler.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace me
{

namespace
{
constexpr const char* kNodePayload = "MATERIAL_GRAPH_NODE";
constexpr size_t kMaxUndoSteps = 100;
constexpr double kStatusSeconds = 6.0;
constexpr float kGamma = 2.2f;

// The panels' colours: a header strip over each, as Unreal's tabs.
constexpr ImU32 kPanelHeader = IM_COL32(40, 40, 43, 255);
constexpr ImU32 kPanelHeaderText = IM_COL32(210, 210, 214, 255);
constexpr ImU32 kSplitter = IM_COL32(18, 18, 20, 255);
constexpr ImU32 kSplitterHover = IM_COL32(255, 160, 40, 150);
constexpr ImU32 kSplitterActive = IM_COL32(255, 160, 40, 255);

std::string BuildMaterialSlotLabel(const ModelImportedMaterialInfo& material, size_t materialIndex)
{
    return material.name.empty() ? ("Material " + std::to_string(materialIndex)) : material.name;
}

// A graph still where the old default layout put it (the Output at (1120, 240), a Surface at (72, 80)),
// sized for the old window's large nodes: never arranged by hand, so laid out afresh as a new one is.
bool UsesLegacyDefaultLayout(const MaterialShaderGraph& graph)
{
    const MaterialShaderNodeLayout legacy{};
    bool output = false;
    bool surface = false;
    for (const MaterialShaderNode& node : graph.nodes)
    {
        const auto at = [&node](const MaterialGraphNodePosition& position)
        {
            return node.position.x == position.x && node.position.y == position.y;
        };
        output |= node.type == MaterialShaderNodeType::Output && at(legacy.outputNode);
        surface |= node.type == MaterialShaderNodeType::Surface && at(legacy.primarySurfaceNode);
    }
    return output && surface;
}

// The model's materials as the editor edits them, each with a compiled shader graph; one default
// material for a model without any. generated: per slot, whether its graph was made here from the
// material rather than read with it, or still has the old default layout.
std::vector<ModelImportedMaterialInfo> BuildEditableMaterials(const LoadedModelData& loadedModel, std::vector<bool>* generated = nullptr)
{
    std::vector<ModelImportedMaterialInfo> materials;
    materials.reserve(loadedModel.materials.size());
    for (const ModelMaterialData& material : loadedModel.materials)
    {
        ModelImportedMaterialInfo importedMaterial = BuildImportedMaterialInfo(material);
        if (generated != nullptr)
        {
            generated->push_back(importedMaterial.shaderGraph.IsEmpty() || UsesLegacyDefaultLayout(importedMaterial.shaderGraph));
        }
        EnsureMaterialShaderGraph(importedMaterial.name, std::nullopt, importedMaterial);
        RepairMaterialGraphLinks(importedMaterial.shaderGraph);
        CompileMaterialShaderGraph(importedMaterial);
        materials.push_back(std::move(importedMaterial));
    }
    if (materials.empty())
    {
        materials.push_back(ModelImportedMaterialInfo{});
        EnsureMaterialShaderGraph(materials.front().name, std::nullopt, materials.front());
    }
    return materials;
}

// The slot most like car paint: one named car paint, then paint or body that is not on a rim, a
// caliper or the interior; 0 when none is.
int FindPaintSlot(const std::vector<ModelImportedMaterialInfo>& materials)
{
    const auto lower = [](std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c)
                       {
                           return static_cast<char>(std::tolower(c));
                       });
        return text;
    };
    int best = -1;
    int bestRank = 0;
    for (size_t index = 0; index < materials.size(); ++index)
    {
        const std::string name = lower(materials[index].name);
        const bool excluded = name.find("rim") != std::string::npos || name.find("caliper") != std::string::npos ||
                              name.find("int_") != std::string::npos || name.find("interior") != std::string::npos;
        int rank = 0;
        if (name.find("carpaint") != std::string::npos || name.find("car_paint") != std::string::npos)
        {
            rank = 3;
        }
        else if (!excluded && name.find("paint") != std::string::npos)
        {
            rank = 2;
        }
        else if (!excluded && name.find("body") != std::string::npos)
        {
            rank = 1;
        }
        if (rank > bestRank)
        {
            best = static_cast<int>(index);
            bestRank = rank;
        }
    }
    return std::max(best, 0);
}

// The Color or Scalar node linked into one of the Output node's inputs, which the compile takes over
// the Output's own factor; nullptr when the input is the factor itself.
MaterialShaderNode* FindLinkedOutputInput(MaterialShaderGraph& graph, uint32_t outputId, std::string_view slot, MaterialShaderNodeType type)
{
    const MaterialShaderLink* link = FindIncomingMaterialGraphLink(graph, outputId, slot);
    if (link == nullptr)
    {
        return nullptr;
    }
    MaterialShaderNode* node = FindMaterialGraphNode(graph, link->fromNodeId);
    return node != nullptr && node->type == type ? node : nullptr;
}

// ---- Panels -----------------------------------------------------------------------------------------

// A panel's title strip, as Unreal's tabs; returns its height.
void PanelHeader(const char* title, float uiScale)
{
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetTextLineHeight() + 8.0f * uiScale;
    ImDrawList& drawList = *ImGui::GetWindowDrawList();
    drawList.AddRectFilled(min, ImVec2(min.x + width, min.y + height), kPanelHeader);
    drawList.AddText(ImVec2(min.x + 8.0f * uiScale, min.y + 4.0f * uiScale), kPanelHeaderText, title);
    ImGui::Dummy(ImVec2(width, height));
}

// A bar between two panels; returns how far it was dragged this frame along its axis.
float Splitter(const char* id, const ImVec2& min, const ImVec2& size, bool vertical)
{
    ImGui::SetCursorScreenPos(min);
    ImGui::InvisibleButton(id, ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (hovered || active)
    {
        ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    }
    ImDrawList& drawList = *ImGui::GetWindowDrawList();
    drawList.AddRectFilled(min, ImVec2(min.x + size.x, min.y + size.y), active ? kSplitterActive : (hovered ? kSplitterHover : kSplitter));
    if (!active)
    {
        return 0.0f;
    }
    return vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
}

// Every child of the layout: never moves the window, never scrolls unless it says so.
constexpr ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
constexpr ImGuiWindowFlags kScrollingPanelFlags = ImGuiWindowFlags_NoMove;

// A toolbar button: an icon and a label, highlighted while `on`.
bool ToolbarButton(const char* icon, const char* label, const char* tooltip, bool enabled = true, bool on = false)
{
    ImGui::BeginDisabled(!enabled);
    if (on)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    const std::string text = std::string(icon) + "  " + label;
    const bool pressed = ImGui::Button(text.c_str());
    if (on)
    {
        ImGui::PopStyleColor();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", tooltip);
    return pressed;
}

void ToolbarSeparator()
{
    ImGui::SameLine();
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(position.x, position.y + 3.0f), ImVec2(position.x, position.y + height - 3.0f), ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::Dummy(ImVec2(1.0f, height));
    ImGui::SameLine();
}

// ---- Property grid (Unreal's Details: a label column and a value column) ---------------------------

bool BeginProperties(const char* id)
{
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX))
    {
        return false;
    }
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
    return true;
}

void PropertyLabel(const char* label, const char* tooltip = nullptr)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip != nullptr)
    {
        ImGui::SetItemTooltip("%s", tooltip);
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

bool PropertyFloat(const char* label, float* value, float minimum, float maximum, const char* format = "%.2f", const char* tooltip = nullptr)
{
    PropertyLabel(label, tooltip);
    ImGui::PushID(label);
    const bool changed = DragFloatInRange("##value", value, minimum, maximum, format);
    ImGui::PopID();
    return changed;
}

bool PropertyCheckbox(const char* label, bool* value, const char* tooltip = nullptr)
{
    PropertyLabel(label, tooltip);
    ImGui::PushID(label);
    const bool changed = ImGui::Checkbox("##value", value);
    ImGui::PopID();
    return changed;
}

// A linear colour edited as the picker shows colours (gamma 2.2), so a mid grey reads as one.
bool ColorEditLinear(const char* id, float* linear, int components, ImGuiColorEditFlags flags = 0)
{
    float display[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    for (int channel = 0; channel < 3; ++channel)
    {
        display[channel] = std::pow(std::max(linear[channel], 0.0f), 1.0f / kGamma);
    }
    if (components == 4)
    {
        display[3] = linear[3];
    }
    const bool changed = components == 4 ? ImGui::ColorEdit4(id, display, flags | ImGuiColorEditFlags_AlphaBar)
                                         : ImGui::ColorEdit3(id, display, flags);
    if (changed)
    {
        for (int channel = 0; channel < 3; ++channel)
        {
            linear[channel] = std::pow(std::max(display[channel], 0.0f), kGamma);
        }
        if (components == 4)
        {
            linear[3] = display[3];
        }
    }
    return changed;
}

bool PropertyColor(const char* label, float* linear, int components, ImGuiColorEditFlags flags = 0, const char* tooltip = nullptr)
{
    PropertyLabel(label, tooltip);
    ImGui::PushID(label);
    const bool changed = ColorEditLinear("##value", linear, components, flags);
    ImGui::PopID();
    return changed;
}

// The Output node's inputs that a linked node overrides, by the factor they stand for: shown
// disabled, with the reason.
struct LinkedInputs
{
    MaterialShaderGraph* graph = nullptr;
    uint32_t outputId = 0;

    bool IsLinked(std::string_view slot) const
    {
        return graph != nullptr && FindIncomingMaterialGraphLink(*graph, outputId, slot) != nullptr;
    }
};

bool PropertyFactor(const LinkedInputs& linked, std::string_view slot, const char* label, float* value, float minimum, float maximum, const char* format = "%.2f")
{
    const bool overridden = linked.IsLinked(slot);
    ImGui::BeginDisabled(overridden);
    const bool changed = PropertyFloat(label, value, minimum, maximum, format, overridden ? "A node linked into this input sets it." : nullptr);
    ImGui::EndDisabled();
    return changed;
}

// The Output node's material properties, in Unreal's categories.
bool DrawOutputProperties(MaterialPbrSurfaceSettings& pbr, const LinkedInputs& linked)
{
    bool changed = false;
    if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen) && BeginProperties("##material"))
    {
        PropertyLabel("Blend Mode", "Opaque, Masked (cut out below the alpha cutoff) or Translucent (blended over what is behind).");
        const char* modes[] = {"Opaque", "Masked", "Translucent"};
        int mode = static_cast<int>(pbr.alphaMode);
        if (ImGui::Combo("##blend_mode", &mode, modes, 3))
        {
            pbr.alphaMode = static_cast<MaterialAlphaMode>(mode);
            changed = true;
        }
        if (pbr.alphaMode == MaterialAlphaMode::Mask)
        {
            changed |= PropertyFloat("Alpha Cutoff", &pbr.alphaCutoff, 0.0f, 1.0f);
        }
        changed |= PropertyFactor(linked, "opacity", "Opacity", &pbr.opacity, 0.0f, 1.0f);
        changed |= PropertyCheckbox("Unlit", &pbr.unlit, "The base colour alone, at the display's paper white (KHR_materials_unlit).");
        changed |= PropertyCheckbox("Deferred Decal", &pbr.decal, "Translucent only: blended into the surface under it, which is then lit once with it.");
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Surface", ImGuiTreeNodeFlags_DefaultOpen) && BeginProperties("##surface"))
    {
        {
            const bool overridden = linked.IsLinked("base_factor");
            ImGui::BeginDisabled(overridden);
            changed |= PropertyColor("Base Color", pbr.baseColorFactor, 4, ImGuiColorEditFlags_Float, overridden ? "A node linked into this input sets it." : nullptr);
            ImGui::EndDisabled();
        }
        changed |= PropertyFactor(linked, "metallic_factor", "Metallic", &pbr.metallicFactor, 0.0f, 1.0f);
        changed |= PropertyFactor(linked, "roughness_factor", "Roughness", &pbr.roughnessFactor, 0.0f, 1.0f);
        changed |= PropertyFactor(linked, "normal_scale", "Normal Scale", &pbr.normalScale, 0.0f, 4.0f);
        changed |= PropertyFactor(linked, "ao_strength", "AO Strength", &pbr.occlusionStrength, 0.0f, 1.0f);
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Emission", ImGuiTreeNodeFlags_DefaultOpen) && BeginProperties("##emission"))
    {
        {
            const bool overridden = linked.IsLinked("emissive_color");
            ImGui::BeginDisabled(overridden);
            changed |= PropertyColor("Emissive Color", pbr.emissiveColor, 3, ImGuiColorEditFlags_Float, overridden ? "A node linked into this input sets it." : nullptr);
            ImGui::EndDisabled();
        }
        changed |= PropertyFactor(linked, "emissive_intensity", "Intensity (cd/m2)", &pbr.emissiveIntensity, 0.0f, 100000.0f, "%.4g");
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Clear Coat", ImGuiTreeNodeFlags_DefaultOpen) && BeginProperties("##clearcoat"))
    {
        changed |= PropertyFloat("Clear Coat", &pbr.clearcoatFactor, 0.0f, 1.0f);
        changed |= PropertyFloat("Coat Roughness", &pbr.clearcoatRoughnessFactor, 0.0f, 1.0f);
        changed |= PropertyFloat("Coat Normal Scale", &pbr.clearcoatNormalScale, 0.0f, 4.0f);
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Sheen") && BeginProperties("##sheen"))
    {
        changed |= PropertyColor("Sheen Color", pbr.sheenColorFactor, 3);
        changed |= PropertyFloat("Sheen Roughness", &pbr.sheenRoughnessFactor, 0.0f, 1.0f);
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Anisotropy") && BeginProperties("##anisotropy"))
    {
        changed |= PropertyFloat("Anisotropy", &pbr.anisotropyStrength, 0.0f, 1.0f);
        // Stored in radians as glTF has it; edited in degrees.
        float degrees = pbr.anisotropyRotation * (180.0f / 3.14159265f);
        PropertyLabel("Rotation");
        if (ImGui::DragFloat("##rotation", &degrees, 1.0f, -180.0f, 180.0f, "%.1f deg"))
        {
            pbr.anisotropyRotation = degrees * (3.14159265f / 180.0f);
            changed = true;
        }
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Specular") && BeginProperties("##specular"))
    {
        PropertyLabel("IOR", "Index of refraction (KHR_materials_ior): 1.5 is the usual dielectric; 0 an infinite index.");
        if (ImGui::DragFloat("##ior", &pbr.ior, 0.01f, 0.0f, 4.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp))
        {
            pbr.ior = SanitizeIor(pbr.ior);
            changed = true;
        }
        changed |= PropertyFloat("Specular", &pbr.specularFactor, 0.0f, 1.0f);
        changed |= PropertyColor("Specular Color", pbr.specularColorFactor, 3, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Iridescence") && BeginProperties("##iridescence"))
    {
        changed |= PropertyFloat("Iridescence", &pbr.iridescenceFactor, 0.0f, 1.0f);
        changed |= PropertyFloat("Film IOR", &pbr.iridescenceIor, 1.0f, 3.0f);
        changed |= PropertyFloat("Thickness Min (nm)", &pbr.iridescenceThicknessMinimum, 0.0f, 2000.0f, "%.0f");
        changed |= PropertyFloat("Thickness Max (nm)", &pbr.iridescenceThicknessMaximum, 0.0f, 2000.0f, "%.0f");
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Transmission") && BeginProperties("##transmission"))
    {
        changed |= PropertyFloat("Transmission", &pbr.transmissionFactor, 0.0f, 1.0f);
        changed |= PropertyFloat("Thickness", &pbr.thicknessFactor, 0.0f, 10.0f, "%.3f", "The volume's thickness in mesh units; 0 is a thin wall.");
        changed |= PropertyFloat("Attenuation (m)", &pbr.attenuationDistance, 0.0f, 100.0f, "%.3f", "Metres after which white light turns the attenuation colour; 0 absorbs nothing.");
        changed |= PropertyColor("Attenuation Color", pbr.attenuationColor, 3);
        changed |= PropertyFloat("Dispersion", &pbr.dispersion, 0.0f, 10.0f);
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Diffuse Transmission") && BeginProperties("##diffuse_transmission"))
    {
        changed |= PropertyFloat("Factor", &pbr.diffuseTransmissionFactor, 0.0f, 1.0f);
        changed |= PropertyColor("Color", pbr.diffuseTransmissionColor, 3);
        changed |= PropertyCheckbox("Volume Scatter", &pbr.volumeScatter);
        changed |= PropertyColor("Multi-scatter Color", pbr.multiscatterColor, 3);
        changed |= PropertyFloat("Scatter Anisotropy", &pbr.scatterAnisotropy, -kMaxScatterAnisotropy, kMaxScatterAnisotropy);
        ImGui::EndTable();
    }
    return changed;
}

// The factors a colour change needs, without the graph. They are written where the compile reads
// them: the Output node's own factors, or the Color or Scalar node linked into them.
bool DrawQuickEdit(ModelImportedMaterialInfo& material, float& brightness)
{
    MaterialShaderNode* output = FindMaterialGraphOutputNode(material.shaderGraph);
    if (output == nullptr)
    {
        ImGui::TextDisabled("The graph has no Output node.");
        return false;
    }
    bool changed = false;
    if (!BeginProperties("##quick_edit"))
    {
        return false;
    }
    // The factor is linear. It is edited in gamma, as the picker shows colours, and split into a
    // colour in [0, 1] and a brightness, so a factor above 1 (a kn5 paint's diffuse gain) stays
    // editable. The brightness is kept between frames while the factor fits under it.
    MaterialShaderNode* colorNode = FindLinkedOutputInput(material.shaderGraph, output->id, "base_factor", MaterialShaderNodeType::Color);
    float* factor = colorNode != nullptr ? colorNode->colorValue : output->pbr.baseColorFactor;
    float gamma[3];
    for (size_t channel = 0; channel < 3; ++channel)
    {
        gamma[channel] = std::pow(std::max(factor[channel], 0.0f), 1.0f / kGamma);
    }
    const float peak = std::max({gamma[0], gamma[1], gamma[2]});
    if (brightness <= 0.0f || peak > brightness * 1.0001f)
    {
        brightness = std::max(1.0f, peak);
    }
    float color[3] = {gamma[0] / brightness, gamma[1] / brightness, gamma[2] / brightness};
    PropertyLabel("Base Color", material.baseColorTexturePath.empty() ? nullptr : "Multiplies the base colour map.");
    bool colorChanged = ImGui::ColorEdit3("##quick_color", color, ImGuiColorEditFlags_PickerHueWheel);
    colorChanged |= PropertyFloat("Brightness", &brightness, 0.05f, 4.0f, "%.2f", "Scales the colour past 1, as a kn5 paint's diffuse gain does.");
    if (colorChanged)
    {
        for (size_t channel = 0; channel < 3; ++channel)
        {
            factor[channel] = std::pow(std::clamp(color[channel], 0.0f, 1.0f) * brightness, kGamma);
        }
        changed = true;
    }
    const auto scalarInput = [&](std::string_view slot, float& outputFactor) -> float&
    {
        MaterialShaderNode* node = FindLinkedOutputInput(material.shaderGraph, output->id, slot, MaterialShaderNodeType::Scalar);
        return node != nullptr ? node->scalarValue : outputFactor;
    };
    changed |= PropertyFloat("Metallic", &scalarInput("metallic_factor", output->pbr.metallicFactor), 0.0f, 1.0f);
    changed |= PropertyFloat("Roughness", &scalarInput("roughness_factor", output->pbr.roughnessFactor), 0.0f, 1.0f);
    changed |= PropertyFloat("Clear Coat", &output->pbr.clearcoatFactor, 0.0f, 1.0f);
    changed |= PropertyFloat("Coat Roughness", &output->pbr.clearcoatRoughnessFactor, 0.0f, 1.0f);
    ImGui::EndTable();
    return changed;
}

// What the material resolved to: its textures and every factor.
void DrawResolvedMaterial(const ModelImportedMaterialInfo& material)
{
    DrawPrimaryMaterialTextureRows(material);
    if (BeginProperties("##resolved"))
    {
        const auto row = [](const char* label, const char* format, auto... values)
        {
            PropertyLabel(label);
            ImGui::Text(format, values...);
        };
        const MaterialPbrSurfaceSettings& pbr = material.pbr;
        row("Base Color", "%.3f %.3f %.3f %.3f", pbr.baseColorFactor[0], pbr.baseColorFactor[1], pbr.baseColorFactor[2], pbr.baseColorFactor[3]);
        row("Metallic / Roughness", "%.2f / %.2f", pbr.metallicFactor, pbr.roughnessFactor);
        row("Normal / AO", "%.2f / %.2f", pbr.normalScale, pbr.occlusionStrength);
        row("Emissive", "%.2f %.2f %.2f x %.4g", pbr.emissiveColor[0], pbr.emissiveColor[1], pbr.emissiveColor[2], pbr.emissiveIntensity);
        row("Opacity", "%.2f (%s)", pbr.opacity, ToString(pbr.alphaMode));
        row("Clear Coat", "%.2f, roughness %.2f", pbr.clearcoatFactor, pbr.clearcoatRoughnessFactor);
        row("Sheen", "%.2f %.2f %.2f, roughness %.2f", pbr.sheenColorFactor[0], pbr.sheenColorFactor[1], pbr.sheenColorFactor[2], pbr.sheenRoughnessFactor);
        row("IOR / Specular", "%.3f / %.2f", pbr.ior, pbr.specularFactor);
        row("Transmission", "%.2f", pbr.transmissionFactor);
        ImGui::EndTable();
    }
    if (HasSecondaryMaterialLayer(material.blendGraph))
    {
        ImGui::SeparatorText("Layer B");
        DrawSecondaryMaterialTextureRows(material.blendGraph);
    }
}

std::string FormatCount(uint64_t value)
{
    std::string digits = std::to_string(value);
    for (int position = static_cast<int>(digits.size()) - 3; position > 0; position -= 3)
    {
        digits.insert(static_cast<size_t>(position), ",");
    }
    return digits;
}
}

struct MaterialEditorWindow::GeometryJob : enki::ITaskSet
{
    std::vector<MaterialPreviewMesh> meshes;
    std::shared_ptr<const MaterialPreviewGeometry> result;

    void ExecuteRange(enki::TaskSetPartition range, uint32_t threadNumber) override
    {
        static_cast<void>(range);
        static_cast<void>(threadNumber);
        result = MaterialPreviewGeometry::Build(std::move(meshes));
    }
};

MaterialEditorWindow::MaterialEditorWindow()
    : EditorWindow("model_preview", "Material Editor", ICON_PH_PAINT_BRUSH)
{
    m_shapeCamera.target = glm::vec3(0.0f);
    m_shapeCamera.yaw = 0.6f;
    m_shapeCamera.pitch = 0.35f;
    m_shapeCamera.distance = 2.2f;
}

MaterialEditorWindow::~MaterialEditorWindow()
{
    if (m_geometryJob && TaskSystem::IsRunning())
    {
        TaskSystem::Scheduler().WaitforTask(m_geometryJob.get());
    }
}

void MaterialEditorWindow::OpenModel(EditorContext& context, const std::string& modelPath, bool preselectPaint)
{
    RevertScenePreview(context);
    LoadModel(modelPath, preselectPaint);
    context.windows.Open(*this);
}

void MaterialEditorWindow::LoadModel(const std::string& modelPath, bool preselectPaint)
{
    const std::filesystem::path normalizedPath = NormalizeFilesystemPath(modelPath);
    Reset();
    std::vector<bool> generatedGraphs;
    m_modelPath = normalizedPath.string();
    m_displayName = normalizedPath.filename().string();
    try
    {
        auto loadedModel = std::make_shared<LoadedModelData>(ModelLoader::LoadModelAsImported(m_modelPath));
        m_importedMaterials = BuildEditableMaterials(*loadedModel);
        ModelLoader::ApplyMaterialDefinitions(m_modelPath, *loadedModel);
        std::vector<bool> generated;
        m_materials = BuildEditableMaterials(*loadedModel, &generated);
        generatedGraphs = std::move(generated);
        m_model = std::move(loadedModel);
        m_importedMaterials.resize(m_materials.size());
        m_slotAsImported.assign(m_materials.size(), true);
        m_stashedEdits.assign(m_materials.size(), std::nullopt);
        // A slot with a definition of its own (by index, or by name as ModelLoader also applies
        // them) shows an edit.
        for (const std::filesystem::path& file : FindMaterialDefinitionFiles(m_modelPath))
        {
            const std::optional<std::string> name = ReadMaterialDefinitionName(file);
            const std::optional<uint32_t> index = MaterialDefinitionIndex(m_modelPath, file);
            for (size_t slot = 0; slot < m_materials.size(); ++slot)
            {
                const std::string& slotName = m_materials[slot].name;
                if ((index == slot && (!name.has_value() || *name == slotName)) || (!slotName.empty() && name == slotName))
                {
                    m_slotAsImported[slot] = false;
                }
            }
        }
        m_slotSubmeshes.assign(m_materials.size(), 0);
        m_slotTriangles.assign(m_materials.size(), 0);
        for (const ModelSubmeshData& submesh : m_model->submeshes)
        {
            if (submesh.materialIndex < m_materials.size())
            {
                ++m_slotSubmeshes[submesh.materialIndex];
                m_slotTriangles[submesh.materialIndex] += submesh.mesh.indices.size() / 3;
            }
        }
        if (preselectPaint)
        {
            m_selectedSlot = FindPaintSlot(m_materials);
        }
    }
    catch (const std::exception& error)
    {
        m_loadError = error.what();
        m_materials.clear();
    }
    m_slotUi.assign(m_materials.size(), SlotUi{});
    for (size_t slot = 0; slot < m_slotUi.size() && slot < generatedGraphs.size(); ++slot)
    {
        m_slotUi[slot].arrangeOnShow = generatedGraphs[slot];
    }
    m_modelCamera = MaterialPreviewCamera{};
    // Cars face -Z: the front three-quarter view, as a showroom shows them.
    m_modelCamera.yaw = 2.45f;
    m_modelCamera.pitch = 0.28f;
    m_shape = MaterialPreviewShape::Model;
    RequestPreviewGeometry();
}

void MaterialEditorWindow::Reset()
{
    if (m_geometryJob && TaskSystem::IsRunning())
    {
        TaskSystem::Scheduler().WaitforTask(m_geometryJob.get());
    }
    m_geometryJob.reset();
    m_modelPath.clear();
    m_displayName.clear();
    m_loadError.clear();
    m_model.reset();
    m_materials.clear();
    m_importedMaterials.clear();
    m_slotAsImported.clear();
    m_stashedEdits.clear();
    m_editedSlots.clear();
    m_slotSubmeshes.clear();
    m_slotTriangles.clear();
    m_slotUi.clear();
    m_selectedSlot = 0;
    m_scenePreviewed = false;
    m_quickEditBrightness = 0.0f;
    m_frameStart.reset();
    m_undoStepOpen = false;
    m_status.clear();
    m_graph.CancelInteraction();
    m_pickTextureNode = 0;
    m_pickTextureRequested = false;
    m_nodeNameFor = 0;
    m_modelGeometry.reset();
    m_preview.SetScene(nullptr, nullptr);
    m_previewImage.Reset();
    m_thumbnails.clear();
    m_textures.Clear();
    m_previewMaterialsDirty = true;
}

void MaterialEditorWindow::OnAssetRenamed(const std::string& oldPath, const std::string& newPath)
{
    if (const std::optional<std::filesystem::path> rebased = AssetPaths::Rebase(m_modelPath, oldPath, newPath))
    {
        m_modelPath = rebased->string();
        m_displayName = rebased->filename().string();
    }
}

void MaterialEditorWindow::RevertScenePreview(EditorContext& context)
{
    if (m_scenePreviewed)
    {
        context.result.actions.revertImportedModelMaterials = m_modelPath;
        m_scenePreviewed = false;
    }
}

void MaterialEditorWindow::Tick(EditorContext& context)
{
    static_cast<void>(context);
    EditorUserTexture::CollectRetired();
    // Close once the model is gone. Checking the disk every frame is a filesystem call per frame
    // for a file that rarely changes, so it is checked twice a second.
    constexpr double kExistsCheckIntervalSeconds = 0.5;
    const double now = ImGui::GetTime();
    if (!IsOpen() || now - m_lastExistsCheckTime < kExistsCheckIntervalSeconds)
    {
        return;
    }
    m_lastExistsCheckTime = now;
    std::error_code errorCode;
    const std::filesystem::path modelPath(m_modelPath);
    if (m_modelPath.empty() || !std::filesystem::exists(modelPath, errorCode) || errorCode || !IsSupportedModelAssetPath(modelPath))
    {
        // Nothing to read back from disk: the scene's copies went with the file.
        m_scenePreviewed = false;
        Close();
    }
}

void MaterialEditorWindow::OnClose(EditorContext& context)
{
    RevertScenePreview(context);
    Reset();
}

void MaterialEditorWindow::PreBegin(EditorContext& context)
{
    static_cast<void>(context);
    const float scale = UiScale();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(
        ImVec2(std::min(1600.0f * scale, viewport->WorkSize.x * 0.92f), std::min(940.0f * scale, viewport->WorkSize.y * 0.9f)),
        ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(900.0f * scale, 560.0f * scale), ImVec2(FLT_MAX, FLT_MAX));
}

ImGuiWindowFlags MaterialEditorWindow::GetWindowFlags(const EditorContext& context) const
{
    static_cast<void>(context);
    // The panels scroll themselves; the window itself never does. Unsaved edits mark the title.
    return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | (IsDirty() ? ImGuiWindowFlags_UnsavedDocument : 0);
}

void MaterialEditorWindow::SetStatus(std::string message)
{
    m_status = std::move(message);
    m_statusTime = std::chrono::steady_clock::now();
}

void MaterialEditorWindow::SelectSlot(int slot)
{
    if (m_materials.empty())
    {
        return;
    }
    slot = std::clamp(slot, 0, static_cast<int>(m_materials.size()) - 1);
    if (slot == m_selectedSlot)
    {
        return;
    }
    m_selectedSlot = slot;
    m_quickEditBrightness = 0.0f;
    m_undoStepOpen = false;
    m_graph.CancelInteraction();
    m_nodeNameFor = 0;
    if (m_shape != MaterialPreviewShape::Model)
    {
        m_previewMaterialsDirty = true;
    }
}

// ---- Edits and undo -------------------------------------------------------------------------------

MaterialEditorWindow::SlotState MaterialEditorWindow::CaptureSlot(int slot) const
{
    const size_t index = static_cast<size_t>(slot);
    return SlotState{m_materials[index], m_slotAsImported[index], m_stashedEdits[index]};
}

void MaterialEditorWindow::RestoreSlot(int slot, const SlotState& state)
{
    const size_t index = static_cast<size_t>(slot);
    m_materials[index] = state.material;
    m_slotAsImported[index] = state.asImported;
    m_stashedEdits[index] = state.stashedEdit;
    m_quickEditBrightness = 0.0f;
    m_nodeNameFor = 0;
}

void MaterialEditorWindow::BeginFrameEdits()
{
    if (m_materials.empty())
    {
        m_frameStart.reset();
        return;
    }
    // Copied only while an edit could start: a cheap struct, and the frame needs it before it knows.
    if (!m_undoStepOpen)
    {
        m_frameStart = CaptureSlot(m_selectedSlot);
    }
}

void MaterialEditorWindow::EndFrameEdits(EditorContext& context, bool changed, bool dragging)
{
    if (m_materials.empty())
    {
        return;
    }
    SlotUi& ui = m_slotUi[static_cast<size_t>(m_selectedSlot)];
    if (changed)
    {
        // One undo step for everything until the mouse lets go: a drag, a slider, a colour pick.
        if (!m_undoStepOpen && m_frameStart.has_value())
        {
            ui.undo.push_back(std::move(*m_frameStart));
            if (ui.undo.size() > kMaxUndoSteps)
            {
                ui.undo.erase(ui.undo.begin());
            }
            ui.redo.clear();
            m_undoStepOpen = true;
        }
        // An edit starts from what is shown, the imported material included.
        m_slotAsImported[static_cast<size_t>(m_selectedSlot)] = false;
        m_stashedEdits[static_cast<size_t>(m_selectedSlot)].reset();
        OnMaterialChanged(context, m_selectedSlot);
    }
    const bool holding = dragging || ImGui::IsAnyItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (!holding)
    {
        m_undoStepOpen = false;
    }
}

void MaterialEditorWindow::OnMaterialChanged(EditorContext& context, int slot)
{
    ModelImportedMaterialInfo& material = m_materials[static_cast<size_t>(slot)];
    const MaterialGraphCompileResult compiled = CompileMaterialShaderGraph(material);
    if (!compiled.message.empty())
    {
        SetStatus(compiled.message);
    }
    m_editedSlots.insert(static_cast<uint32_t>(slot));
    m_previewMaterialsDirty = true;
    if (m_livePreview)
    {
        context.result.actions.previewImportedModelMaterial =
            EditorUiActions::ImportedModelMaterialPreview{m_modelPath, {{static_cast<uint32_t>(slot), material}}};
        m_scenePreviewed = true;
    }
}

bool MaterialEditorWindow::Undo()
{
    if (m_materials.empty())
    {
        return false;
    }
    SlotUi& ui = m_slotUi[static_cast<size_t>(m_selectedSlot)];
    if (ui.undo.empty())
    {
        return false;
    }
    ui.redo.push_back(CaptureSlot(m_selectedSlot));
    RestoreSlot(m_selectedSlot, ui.undo.back());
    ui.undo.pop_back();
    m_undoStepOpen = false;
    m_frameStart.reset();
    CompileMaterialShaderGraph(m_materials[static_cast<size_t>(m_selectedSlot)]);
    m_editedSlots.insert(static_cast<uint32_t>(m_selectedSlot));
    m_previewMaterialsDirty = true;
    SetStatus("Undo");
    return true;
}

bool MaterialEditorWindow::Redo()
{
    if (m_materials.empty())
    {
        return false;
    }
    SlotUi& ui = m_slotUi[static_cast<size_t>(m_selectedSlot)];
    if (ui.redo.empty())
    {
        return false;
    }
    ui.undo.push_back(CaptureSlot(m_selectedSlot));
    RestoreSlot(m_selectedSlot, ui.redo.back());
    ui.redo.pop_back();
    m_undoStepOpen = false;
    m_frameStart.reset();
    CompileMaterialShaderGraph(m_materials[static_cast<size_t>(m_selectedSlot)]);
    m_editedSlots.insert(static_cast<uint32_t>(m_selectedSlot));
    m_previewMaterialsDirty = true;
    SetStatus("Redo");
    return true;
}

void MaterialEditorWindow::SetSlotAsImported(EditorContext& context, int slot, bool asImported)
{
    const size_t index = static_cast<size_t>(slot);
    if (m_slotAsImported[index] == asImported || (!asImported && !m_stashedEdits[index].has_value()))
    {
        return;
    }
    SlotUi& ui = m_slotUi[index];
    ui.undo.push_back(CaptureSlot(slot));
    ui.redo.clear();
    if (asImported)
    {
        m_stashedEdits[index] = m_materials[index];
        m_materials[index] = m_importedMaterials[index];
        m_slotUi[index].arrangeOnShow = true;
    }
    else
    {
        m_materials[index] = *m_stashedEdits[index];
        m_stashedEdits[index].reset();
    }
    m_slotAsImported[index] = asImported;
    m_quickEditBrightness = 0.0f;
    m_nodeNameFor = 0;
    m_slotUi[index].selection = MaterialGraphSelection{};
    OnMaterialChanged(context, slot);
}

void MaterialEditorWindow::Save(EditorContext& context)
{
    if (m_editedSlots.empty())
    {
        return;
    }
    EditorUiActions::ImportedModelMaterialsUpdate update{m_modelPath, m_materials, {}, {}};
    for (const uint32_t slot : m_editedSlots)
    {
        (m_slotAsImported[slot] ? update.restoredIndices : update.indices).push_back(slot);
    }
    context.result.actions.updatedImportedModelMaterials = std::move(update);
    SetStatus("Saved " + std::to_string(m_editedSlots.size()) + (m_editedSlots.size() == 1 ? " material" : " materials") + " beside " + m_displayName);
    m_editedSlots.clear();
    m_scenePreviewed = false;
}

// ---- The frame ------------------------------------------------------------------------------------

void MaterialEditorWindow::OnGui(EditorContext& context)
{
    if (m_materials.empty())
    {
        ImGui::TextDisabled("%s", m_loadError.empty() ? "No model is open." : m_loadError.c_str());
        return;
    }
    m_selectedSlot = std::clamp(m_selectedSlot, 0, static_cast<int>(m_materials.size()) - 1);
    BeginFrameEdits();
    const int frameSlot = m_selectedSlot;
    HandleShortcuts(context);

    const float scale = UiScale();
    DrawToolbar(context);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float splitter = std::round(5.0f * scale);
    const float statusHeight = ImGui::GetTextLineHeight() + 8.0f * scale;
    const float bodyHeight = std::max(available.y - statusHeight - 2.0f * scale, 120.0f * scale);
    if (m_leftWidth <= 0.0f)
    {
        m_leftWidth = std::round(available.x * 0.29f);
        m_rightWidth = std::round(available.x * 0.17f);
    }
    const float minimumGraph = 280.0f * scale;
    m_rightWidth = std::clamp(m_rightWidth, 170.0f * scale, std::max(170.0f * scale, available.x - 240.0f * scale - minimumGraph - 2.0f * splitter));
    m_leftWidth = std::clamp(m_leftWidth, 240.0f * scale, std::max(240.0f * scale, available.x - m_rightWidth - minimumGraph - 2.0f * splitter));
    const float graphWidth = std::max(available.x - m_leftWidth - m_rightWidth - 2.0f * splitter, 1.0f);

    // Left: the viewport over the details.
    ImGui::SetCursorScreenPos(origin);
    if (ImGui::BeginChild("##left", ImVec2(m_leftWidth, bodyHeight), ImGuiChildFlags_None, kPanelFlags))
    {
        const ImVec2 leftOrigin = ImGui::GetCursorScreenPos();
        const float viewportHeight = std::clamp(std::round(bodyHeight * m_viewportFraction), 140.0f * scale, std::max(bodyHeight - 160.0f * scale, 140.0f * scale));
        ImGui::SetCursorScreenPos(leftOrigin);
        if (ImGui::BeginChild("##viewport", ImVec2(m_leftWidth, viewportHeight), ImGuiChildFlags_None, kPanelFlags))
        {
            DrawViewport(context);
        }
        ImGui::EndChild();
        const float dragged = Splitter("##split_viewport", ImVec2(leftOrigin.x, leftOrigin.y + viewportHeight), ImVec2(m_leftWidth, splitter), false);
        m_viewportFraction = std::clamp(m_viewportFraction + dragged / bodyHeight, 0.15f, 0.85f);
        ImGui::SetCursorScreenPos(ImVec2(leftOrigin.x, leftOrigin.y + viewportHeight + splitter));
        if (ImGui::BeginChild("##details", ImVec2(m_leftWidth, std::max(bodyHeight - viewportHeight - splitter, 1.0f)), ImGuiChildFlags_None, kPanelFlags))
        {
            DrawDetails(context);
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();

    m_leftWidth += Splitter("##split_left", ImVec2(origin.x + m_leftWidth, origin.y), ImVec2(splitter, bodyHeight), true);

    // Middle: the graph.
    ImGui::SetCursorScreenPos(ImVec2(origin.x + m_leftWidth + splitter, origin.y));
    if (ImGui::BeginChild("##graph", ImVec2(graphWidth, bodyHeight), ImGuiChildFlags_None, kPanelFlags))
    {
        DrawGraph(context);
    }
    ImGui::EndChild();

    m_rightWidth -= Splitter("##split_right", ImVec2(origin.x + m_leftWidth + splitter + graphWidth, origin.y), ImVec2(splitter, bodyHeight), true);

    // Right: the slots over the palette.
    const float rightX = origin.x + available.x - m_rightWidth;
    ImGui::SetCursorScreenPos(ImVec2(rightX, origin.y));
    if (ImGui::BeginChild("##right", ImVec2(m_rightWidth, bodyHeight), ImGuiChildFlags_None, kPanelFlags))
    {
        const ImVec2 rightOrigin = ImGui::GetCursorScreenPos();
        const float slotsHeight = std::clamp(std::round(bodyHeight * m_slotsFraction), 120.0f * scale, std::max(bodyHeight - 140.0f * scale, 120.0f * scale));
        if (ImGui::BeginChild("##slots", ImVec2(m_rightWidth, slotsHeight), ImGuiChildFlags_None, kPanelFlags))
        {
            DrawSlots(context);
        }
        ImGui::EndChild();
        const float dragged = Splitter("##split_slots", ImVec2(rightOrigin.x, rightOrigin.y + slotsHeight), ImVec2(m_rightWidth, splitter), false);
        m_slotsFraction = std::clamp(m_slotsFraction + dragged / bodyHeight, 0.15f, 0.85f);
        ImGui::SetCursorScreenPos(ImVec2(rightOrigin.x, rightOrigin.y + slotsHeight + splitter));
        if (ImGui::BeginChild("##palette", ImVec2(m_rightWidth, std::max(bodyHeight - slotsHeight - splitter, 1.0f)), ImGuiChildFlags_None, kPanelFlags))
        {
            DrawPalette(context);
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + bodyHeight + 2.0f * scale));
    DrawStatusBar();

    // The texture file a Texture node asked for (PickFilePath runs its prompt from here every frame).
    bool pickedTexture = false;
    if (const std::optional<std::string> picked = PickFilePath(FileDialogType::OpenTexture, std::exchange(m_pickTextureRequested, false)))
    {
        if (frameSlot == m_selectedSlot)
        {
            if (MaterialShaderNode* node = FindMaterialGraphNode(m_materials[static_cast<size_t>(m_selectedSlot)].shaderGraph, m_pickTextureNode))
            {
                node->texturePath = *picked;
                pickedTexture = true;
                SetStatus("Texture: " + *picked);
            }
        }
    }
    if (pickedTexture && frameSlot == m_selectedSlot)
    {
        EndFrameEdits(context, true, false);
    }
}

void MaterialEditorWindow::HandleShortcuts(EditorContext& context)
{
    // Registered on the window, so while any of its panels has the keyboard these keys are the
    // editor's and not the scene's (Delete would otherwise delete the selected entity).
    const ImGuiIO& io = ImGui::GetIO();
    const bool typing = io.WantTextInput || ImGui::IsAnyItemActive();
    MaterialShaderGraph& graph = m_materials[static_cast<size_t>(m_selectedSlot)].shaderGraph;
    SlotUi& ui = m_slotUi[static_cast<size_t>(m_selectedSlot)];
    bool changed = false;
    const auto pressed = [](ImGuiKeyChord chord)
    {
        return ImGui::Shortcut(chord, ImGuiInputFlags_RouteFocused);
    };
    if (pressed(ImGuiMod_Ctrl | ImGuiKey_Z) && !typing)
    {
        Undo();
        if (m_livePreview)
        {
            OnMaterialChanged(context, m_selectedSlot);
        }
    }
    if ((pressed(ImGuiMod_Ctrl | ImGuiKey_Y) || pressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) && !typing)
    {
        Redo();
        if (m_livePreview)
        {
            OnMaterialChanged(context, m_selectedSlot);
        }
    }
    if (pressed(ImGuiMod_Ctrl | ImGuiKey_S) && !typing)
    {
        Save(context);
    }
    const bool deletePressed = pressed(ImGuiKey_Delete);
    const bool copyPressed = pressed(ImGuiMod_Ctrl | ImGuiKey_C);
    const bool cutPressed = pressed(ImGuiMod_Ctrl | ImGuiKey_X);
    const bool pastePressed = pressed(ImGuiMod_Ctrl | ImGuiKey_V);
    const bool duplicatePressed = pressed(ImGuiMod_Ctrl | ImGuiKey_D);
    const bool selectAllPressed = pressed(ImGuiMod_Ctrl | ImGuiKey_A);
    const bool framePressed = pressed(ImGuiKey_F);
    const bool homePressed = pressed(ImGuiKey_Home);
    if (typing)
    {
        return;
    }
    if (m_graphFocused)
    {
        if (deletePressed)
        {
            changed |= m_graph.DeleteSelection(graph, ui.selection);
        }
        if (copyPressed)
        {
            m_graph.CopySelection(graph, ui.selection);
        }
        if (cutPressed)
        {
            m_graph.CopySelection(graph, ui.selection);
            changed |= m_graph.DeleteSelection(graph, ui.selection);
        }
        if (pastePressed)
        {
            changed |= m_graph.Paste(graph, ui.selection, ui.view);
        }
        if (duplicatePressed)
        {
            changed |= m_graph.DuplicateSelection(graph, ui.selection);
        }
        if (selectAllPressed)
        {
            m_graph.SelectAll(graph, ui.selection);
        }
    }
    if (framePressed)
    {
        if (m_viewportHovered)
        {
            FramePreview();
        }
        else
        {
            m_graph.FrameSelection(ui.selection);
        }
    }
    if (homePressed)
    {
        m_graph.FrameSelection(MaterialGraphSelection{});
    }
    if (changed)
    {
        EndFrameEdits(context, true, false);
        BeginFrameEdits();
    }
}

void MaterialEditorWindow::DrawToolbar(EditorContext& context)
{
    const float scale = UiScale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * scale, 5.0f * scale));
    const float height = ImGui::GetFrameHeight() + 8.0f * scale;
    if (ImGui::BeginChild("##toolbar", ImVec2(0.0f, height), ImGuiChildFlags_None, kPanelFlags))
    {
        ImGui::SetCursorPos(ImVec2(4.0f * scale, 4.0f * scale));
        const size_t slot = static_cast<size_t>(m_selectedSlot);
        if (ToolbarButton(ICON_PH_FLOPPY_DISK, "Save", "Write the edited materials beside the model (Ctrl+S).", IsDirty()))
        {
            Save(context);
        }
        ImGui::SameLine();
        if (ToolbarButton(ICON_PH_ARROW_COUNTER_CLOCKWISE, "Revert", "Read the model's materials from disk again, dropping the edits not saved."))
        {
            const std::string modelPath = m_modelPath;
            RevertScenePreview(context);
            LoadModel(modelPath, false);
            SetStatus("Reverted to the materials on disk");
            ImGui::EndChild();
            ImGui::PopStyleVar();
            return;
        }
        ToolbarSeparator();
        if (ToolbarButton(ICON_PH_ARROW_U_UP_LEFT, "Undo", "Undo the last edit of this slot (Ctrl+Z).", !m_slotUi[slot].undo.empty()))
        {
            Undo();
            OnMaterialChanged(context, m_selectedSlot);
        }
        ImGui::SameLine();
        if (ToolbarButton(ICON_PH_ARROW_U_UP_RIGHT, "Redo", "Redo (Ctrl+Y).", !m_slotUi[slot].redo.empty()))
        {
            Redo();
            OnMaterialChanged(context, m_selectedSlot);
        }
        ToolbarSeparator();
        if (ToolbarButton(ICON_PH_BROADCAST, "Live Preview", "Show edits on the scene's copies of this model as they are made. Save writes them to disk; closing without saving drops them.", true, m_livePreview))
        {
            m_livePreview = !m_livePreview;
            if (!m_livePreview)
            {
                RevertScenePreview(context);
            }
            else if (!m_editedSlots.empty())
            {
                EditorUiActions::ImportedModelMaterialPreview preview{m_modelPath, {}};
                for (const uint32_t edited : m_editedSlots)
                {
                    preview.materials.emplace_back(edited, m_materials[edited]);
                }
                context.result.actions.previewImportedModelMaterial = std::move(preview);
                m_scenePreviewed = true;
            }
        }
        ImGui::SameLine();
        if (ToolbarButton(ICON_PH_SIGN_IN, "Apply to Selection", "Load this model into the entity selected in the scene.", context.scene.HasSelection()))
        {
            context.result.actions.selectedModelPath = m_modelPath;
        }
        ToolbarSeparator();
        // The slot as its import made it, or the edit: switching compares them; Save keeps the one shown.
        const bool asImported = m_slotAsImported[slot];
        if (ToolbarButton(ICON_PH_PACKAGE, "Imported", "Show this slot's material exactly as the import made it (for a car, the livery's own paint). Save removes the edit saved for it.", true, asImported))
        {
            SetSlotAsImported(context, m_selectedSlot, true);
        }
        ImGui::SameLine();
        if (ToolbarButton(ICON_PH_PAINT_BRUSH, "Edited", "Show your edit of this slot. Changing anything makes the slot an edit.", !asImported || m_stashedEdits[slot].has_value(), !asImported))
        {
            SetSlotAsImported(context, m_selectedSlot, false);
        }
        // The model, right aligned.
        const std::string name = m_displayName + (IsDirty() ? " *" : "");
        const float nameWidth = ImGui::CalcTextSize(name.c_str()).x;
        const float right = ImGui::GetWindowContentRegionMax().x - nameWidth - 8.0f * scale;
        ImGui::SameLine();
        if (ImGui::GetCursorPosX() < right)
        {
            ImGui::SetCursorPosX(right);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ui_colors::kTextSecondary, "%s", name.c_str());
            ImGui::SetItemTooltip("%s", m_modelPath.c_str());
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

// ---- Viewport -------------------------------------------------------------------------------------

MaterialPreviewCamera& MaterialEditorWindow::ActiveCamera()
{
    return m_shape == MaterialPreviewShape::Model ? m_modelCamera : m_shapeCamera;
}

void MaterialEditorWindow::FramePreview()
{
    MaterialPreviewCamera& camera = ActiveCamera();
    const std::shared_ptr<const MaterialPreviewGeometry>& geometry =
        m_shape == MaterialPreviewShape::Model ? m_modelGeometry : m_shapeGeometry[static_cast<size_t>(m_shape)];
    if (!geometry || !geometry->HasBounds())
    {
        return;
    }
    camera.target = 0.5f * (geometry->BoundsMin() + geometry->BoundsMax());
    const float radius = std::max(0.5f * glm::length(geometry->BoundsMax() - geometry->BoundsMin()), 1e-3f);
    camera.distance = radius / std::sin(camera.verticalFov * 0.5f) * (m_shape == MaterialPreviewShape::Model ? 0.85f : 1.05f);
}

std::string MaterialEditorWindow::ResolveTexturePath(const std::string& path) const
{
    if (path.empty() || std::filesystem::path(path).is_absolute())
    {
        return path;
    }
    return (std::filesystem::path(m_modelPath).parent_path() / path).lexically_normal().string();
}

void MaterialEditorWindow::RequestPreviewGeometry()
{
    if (!m_model || m_modelGeometry || m_geometryJob)
    {
        return;
    }
    auto job = std::make_unique<GeometryJob>();
    for (const ModelSubmeshData& submesh : m_model->submeshes)
    {
        job->meshes.push_back(MaterialPreviewMesh{std::shared_ptr<const MeshData>(m_model, &submesh.mesh), submesh.materialIndex, submesh.hasTexCoords, submesh.nodeScale});
    }
    job->m_SetSize = 1;
    job->m_Priority = enki::TASK_PRIORITY_MED;
    m_geometryJob = std::move(job);
    if (TaskSystem::IsRunning() && TaskSystem::CanWaitOnCurrentThread())
    {
        TaskSystem::Scheduler().AddTaskSetToPipe(m_geometryJob.get());
    }
    else
    {
        m_geometryJob->ExecuteRange(enki::TaskSetPartition{0, 1}, 0);
    }
}

void MaterialEditorWindow::PollPreviewGeometry()
{
    if (!m_geometryJob || (TaskSystem::IsRunning() && !m_geometryJob->GetIsComplete()))
    {
        return;
    }
    m_modelGeometry = std::move(m_geometryJob->result);
    m_geometryJob.reset();
    m_previewMaterialsDirty = true;
    if (m_shape == MaterialPreviewShape::Model)
    {
        FramePreview();
    }
}

void MaterialEditorWindow::RebuildPreviewMaterials()
{
    m_previewMaterialsDirty = false;
    std::shared_ptr<const MaterialPreviewGeometry> geometry;
    if (m_shape == MaterialPreviewShape::Model)
    {
        geometry = m_modelGeometry;
    }
    else
    {
        std::shared_ptr<const MaterialPreviewGeometry>& shape = m_shapeGeometry[static_cast<size_t>(m_shape)];
        if (!shape)
        {
            std::vector<MaterialPreviewMesh> meshes;
            meshes.push_back(MaterialPreviewMesh{std::make_shared<const MeshData>(BuildMaterialPreviewShapeMesh(m_shape)), 0, true, glm::vec3(1.0f)});
            shape = MaterialPreviewGeometry::Build(std::move(meshes));
        }
        geometry = shape;
    }
    if (!geometry)
    {
        m_preview.SetScene(nullptr, nullptr);
        return;
    }
    // One material per slot and texture coordinates, as the scene converts it (FillRenderSubmeshMaterial).
    std::unordered_map<uint64_t, MaterialPreviewMaterial> converted;
    std::vector<std::string> paths;
    auto materials = std::make_shared<std::vector<MaterialPreviewMaterial>>();
    materials->reserve(geometry->Meshes().size());
    const auto resolve = [this](const std::string& path)
    {
        return ResolveTexturePath(path);
    };
    for (const MaterialPreviewMesh& mesh : geometry->Meshes())
    {
        const uint32_t slot = m_shape == MaterialPreviewShape::Model ? mesh.materialSlot : static_cast<uint32_t>(m_selectedSlot);
        const uint64_t key = (static_cast<uint64_t>(slot) << 1) | (mesh.hasTexCoords ? 1u : 0u);
        auto found = converted.find(key);
        if (found == converted.end())
        {
            ModelMaterialData data = m_model && slot < m_model->materials.size() ? m_model->materials[slot] : ModelMaterialData{};
            if (slot < m_materials.size())
            {
                ApplyImportedMaterialInfo(m_materials[slot], data);
            }
            CpuRenderSubmesh submesh;
            FillRenderSubmeshMaterial(submesh, data, mesh.hasTexCoords, mesh.nodeScale, resolve);
            const std::vector<std::string> submeshPaths = MaterialPreviewTexturePaths(submesh);
            paths.insert(paths.end(), submeshPaths.begin(), submeshPaths.end());
            found = converted.emplace(key, BuildMaterialPreviewMaterial(submesh, m_textures)).first;
        }
        materials->push_back(found->second);
    }
    m_textures.Request(paths);
    m_preview.SetScene(geometry, std::move(materials));
}

void MaterialEditorWindow::UpdatePreview(bool interacting, uint32_t width, uint32_t height)
{
    PollPreviewGeometry();
    if (m_textures.Poll())
    {
        m_previewMaterialsDirty = true;
    }
    if (m_previewMaterialsDirty)
    {
        RebuildPreviewMaterials();
    }
    MaterialPreviewSettings settings = m_previewSettings;
    settings.floor = m_shape == MaterialPreviewShape::Model ? m_modelFloor : m_shapeFloor;
    m_preview.SetHighlightSlot(m_outline && m_shape == MaterialPreviewShape::Model ? m_selectedSlot : -1);
    m_preview.Update(width, height, ActiveCamera(), settings, interacting);
    if (m_preview.TakeNewFrame())
    {
        const MaterialPreviewFrame& frame = m_preview.Frame();
        m_previewImage.Upload(static_cast<int>(frame.width), static_cast<int>(frame.height), frame.rgba.data());
    }
}

void MaterialEditorWindow::FinishPreview()
{
    while (m_geometryJob)
    {
        if (TaskSystem::IsRunning())
        {
            TaskSystem::Scheduler().WaitforTask(m_geometryJob.get());
        }
        PollPreviewGeometry();
    }
    RebuildPreviewMaterials();
    m_textures.WaitForAll();
    RebuildPreviewMaterials();
    if (m_previewWidth > 0 && m_previewHeight > 0)
    {
        MaterialPreviewSettings settings = m_previewSettings;
        settings.floor = m_shape == MaterialPreviewShape::Model ? m_modelFloor : m_shapeFloor;
        m_preview.SetHighlightSlot(m_outline && m_shape == MaterialPreviewShape::Model ? m_selectedSlot : -1);
        m_preview.Finish(m_previewWidth, m_previewHeight, ActiveCamera(), settings);
        const MaterialPreviewFrame& frame = m_preview.Frame();
        m_previewImage.Upload(static_cast<int>(frame.width), static_cast<int>(frame.height), frame.rgba.data());
    }
}

void MaterialEditorWindow::DrawViewport(EditorContext& context)
{
    static_cast<void>(context);
    const float scale = UiScale();
    const ImGuiIO& io = ImGui::GetIO();

    // The header: the title, the shapes, and the preview's settings.
    {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = ImGui::GetFrameHeight() + 6.0f * scale;
        ImGui::GetWindowDrawList()->AddRectFilled(min, ImVec2(min.x + width, min.y + height), kPanelHeader);
        ImGui::SetCursorScreenPos(ImVec2(min.x + 8.0f * scale, min.y + 3.0f * scale));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kPanelHeaderText), "Viewport");
        ImGui::SameLine(0.0f, 12.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5.0f * scale, ImGui::GetStyle().FramePadding.y));
        const struct
        {
            MaterialPreviewShape shape;
            const char* icon;
            const char* tooltip;
        } shapes[] = {
            {MaterialPreviewShape::Model, ICON_PH_CAR, "The whole model, every slot in its material (click a part to select its slot)."},
            {MaterialPreviewShape::Sphere, ICON_PH_SPHERE, "A sphere in the selected slot's material."},
            {MaterialPreviewShape::Cube, ICON_PH_CUBE, "A cube in the selected slot's material."},
            {MaterialPreviewShape::Plane, ICON_PH_SQUARE, "A plane in the selected slot's material."},
            {MaterialPreviewShape::Cylinder, ICON_PH_CYLINDER, "A cylinder in the selected slot's material."}};
        for (const auto& entry : shapes)
        {
            const bool on = m_shape == entry.shape;
            if (on)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            if (ImGui::Button(entry.icon) && !on)
            {
                m_shape = entry.shape;
                m_previewMaterialsDirty = true;
                if (m_shape != MaterialPreviewShape::Model && !m_shapeGeometry[static_cast<size_t>(m_shape)])
                {
                    RebuildPreviewMaterials();
                    FramePreview();
                }
            }
            if (on)
            {
                ImGui::PopStyleColor();
            }
            ImGui::SetItemTooltip("%s: %s", ToString(entry.shape), entry.tooltip);
            ImGui::SameLine(0.0f, 2.0f * scale);
        }
        ImGui::SameLine(0.0f, 10.0f * scale);
        if (ImGui::Button(ICON_PH_CORNERS_OUT))
        {
            FramePreview();
        }
        ImGui::SetItemTooltip("Frame the preview (F)");
        ImGui::SameLine(0.0f, 2.0f * scale);
        if (ImGui::Button(ICON_PH_SLIDERS_HORIZONTAL))
        {
            ImGui::OpenPopup("##preview_settings");
        }
        ImGui::SetItemTooltip("Preview settings: environment, floor, outline, exposure, samples");
        if (ImGui::BeginPopup("##preview_settings"))
        {
            ImGui::SeparatorText("Preview");
            int environment = static_cast<int>(m_previewSettings.environment);
            const char* environments[] = {"Studio", "Daylight"};
            ImGui::SetNextItemWidth(160.0f * scale);
            if (ImGui::Combo("Environment", &environment, environments, 2))
            {
                m_previewSettings.environment = static_cast<MaterialPreviewEnvironmentPreset>(environment);
            }
            bool& floor = m_shape == MaterialPreviewShape::Model ? m_modelFloor : m_shapeFloor;
            ImGui::Checkbox("Floor", &floor);
            ImGui::Checkbox("Outline Selected Slot", &m_outline);
            ImGui::SetNextItemWidth(160.0f * scale);
            DragFloatInRange("Exposure (EV)", &m_previewSettings.exposureEv, -6.0f, 6.0f, "%+.1f");
            int samples = static_cast<int>(m_previewSettings.targetSamples);
            ImGui::SetNextItemWidth(160.0f * scale);
            if (DragIntInRange("Samples", &samples, 1, 1024))
            {
                m_previewSettings.targetSamples = static_cast<uint32_t>(samples);
            }
            ImGui::SetItemTooltip("Samples a pixel gathers before the preview stops refining.");
            if (ImGui::Button("Reset Light"))
            {
                m_previewSettings.lightYaw = 0.0f;
                m_previewSettings.lightPitch = 0.0f;
            }
            ImGui::TextDisabled("Hold L and drag to turn the light.");
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        ImGui::SetCursorScreenPos(ImVec2(min.x, min.y + height));
    }

    // The picture: one button over it takes every mouse button.
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x, 1.0f), std::max(ImGui::GetContentRegionAvail().y, 1.0f));
    const ImVec2 max(min.x + size.x, min.y + size.y);
    ImGui::InvisibleButton("##preview", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    m_viewportHovered = hovered;
    m_viewportMin = min;
    m_viewportMax = max;

    // Unreal's asset viewport: left drag orbits (with L held it turns the light), right drag dollies,
    // middle drag pans, the wheel zooms, a left click picks the slot under it.
    MaterialPreviewCamera& camera = ActiveCamera();
    if (hovered && m_viewportDragButton < 0)
    {
        for (int button : {ImGuiMouseButton_Left, ImGuiMouseButton_Right, ImGuiMouseButton_Middle})
        {
            if (ImGui::IsMouseClicked(button))
            {
                m_viewportDragButton = button;
                m_viewportDragMoved = false;
                break;
            }
        }
    }
    if (m_viewportDragButton >= 0)
    {
        if (ImGui::IsMouseDown(m_viewportDragButton))
        {
            if (ImGui::IsMouseDragging(m_viewportDragButton, 2.0f * scale))
            {
                m_viewportDragMoved = true;
            }
            const ImVec2 delta = io.MouseDelta;
            if (m_viewportDragMoved && (delta.x != 0.0f || delta.y != 0.0f))
            {
                if (m_viewportDragButton == ImGuiMouseButton_Left && ImGui::IsKeyDown(ImGuiKey_L))
                {
                    m_previewSettings.lightYaw += delta.x * 0.01f;
                    m_previewSettings.lightPitch = std::clamp(m_previewSettings.lightPitch - delta.y * 0.01f, -1.4f, 1.4f);
                }
                else if (m_viewportDragButton == ImGuiMouseButton_Left)
                {
                    camera.yaw -= delta.x * 0.008f;
                    camera.pitch = std::clamp(camera.pitch + delta.y * 0.008f, -1.45f, 1.45f);
                }
                else if (m_viewportDragButton == ImGuiMouseButton_Right)
                {
                    camera.distance = std::max(camera.distance * std::exp(delta.y * 0.006f), 0.02f);
                }
                else
                {
                    const glm::vec3 eye = camera.Position();
                    const glm::vec3 forward = glm::normalize(camera.target - eye);
                    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
                    const glm::vec3 up = glm::cross(right, forward);
                    const float perPoint = 2.0f * camera.distance * std::tan(camera.verticalFov * 0.5f) / size.y;
                    camera.target += (-right * delta.x + up * delta.y) * perPoint;
                }
            }
        }
        else
        {
            if (!m_viewportDragMoved && m_viewportDragButton == ImGuiMouseButton_Left && m_shape == MaterialPreviewShape::Model)
            {
                const int slot = m_preview.SlotAt((io.MousePos.x - min.x) / size.x, (io.MousePos.y - min.y) / size.y);
                if (slot >= 0 && slot < static_cast<int>(m_materials.size()))
                {
                    SelectSlot(slot);
                    SetStatus("Selected " + BuildMaterialSlotLabel(m_materials[static_cast<size_t>(slot)], static_cast<size_t>(slot)));
                }
            }
            m_viewportDragButton = -1;
        }
    }
    if (hovered && io.MouseWheel != 0.0f)
    {
        camera.distance = std::max(camera.distance * std::pow(0.88f, io.MouseWheel), 0.02f);
    }

    // A moving camera or a value being dragged renders at half size, to keep up.
    const bool interacting = (m_viewportDragButton >= 0 && m_viewportDragMoved) || ImGui::IsAnyItemActive();
    const ImVec2 pixels(size.x * io.DisplayFramebufferScale.x, size.y * io.DisplayFramebufferScale.y);
    m_previewWidth = static_cast<uint32_t>(std::max(pixels.x, 1.0f));
    m_previewHeight = static_cast<uint32_t>(std::max(pixels.y, 1.0f));
    UpdatePreview(interacting, m_previewWidth, m_previewHeight);

    ImDrawList& drawList = *ImGui::GetWindowDrawList();
    drawList.AddRectFilled(min, max, IM_COL32(20, 20, 22, 255));
    if (m_previewImage.IsValid())
    {
        drawList.AddImage(m_previewImage.Ref(), min, max);
    }
    DrawViewportOverlay(min, max);
}

void MaterialEditorWindow::DrawViewportOverlay(const ImVec2& min, const ImVec2& max)
{
    const float scale = UiScale();
    ImDrawList& drawList = *ImGui::GetWindowDrawList();
    const ImU32 text = IM_COL32(230, 230, 235, 220);
    const ImU32 shadow = IM_COL32(0, 0, 0, 160);
    const auto line = [&](const ImVec2& at, const std::string& value)
    {
        drawList.AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), shadow, value.c_str());
        drawList.AddText(at, text, value.c_str());
    };
    const float lineHeight = ImGui::GetTextLineHeight();
    std::string state;
    if (m_geometryJob)
    {
        state = "Building the preview's hierarchy...";
    }
    else if (m_textures.PendingCount() > 0)
    {
        state = "Loading textures (" + std::to_string(m_textures.PendingCount()) + " to go)...";
    }
    if (!state.empty())
    {
        line(ImVec2(min.x + 10.0f * scale, min.y + 8.0f * scale), state);
    }
    std::string stats;
    if (m_shape == MaterialPreviewShape::Model && m_model)
    {
        const uint64_t triangles = m_modelGeometry ? m_modelGeometry->TriangleCount() : 0;
        stats = FormatCount(triangles) + " triangles, " + std::to_string(m_model->submeshes.size()) + " parts";
    }
    else
    {
        stats = std::string(ToString(m_shape)) + " in " + BuildMaterialSlotLabel(m_materials[static_cast<size_t>(m_selectedSlot)], static_cast<size_t>(m_selectedSlot));
    }
    const uint32_t samples = m_preview.Frame().samples;
    stats += "   " + std::to_string(samples) + " / " + std::to_string(m_previewSettings.targetSamples) + " samples";
    line(ImVec2(min.x + 10.0f * scale, max.y - lineHeight - 8.0f * scale), stats);
    if (ImGui::IsKeyDown(ImGuiKey_L) && m_viewportHovered)
    {
        line(ImVec2(min.x + 10.0f * scale, max.y - 2.0f * lineHeight - 12.0f * scale), "Drag to turn the light");
    }
}

// ---- Details --------------------------------------------------------------------------------------

void MaterialEditorWindow::DrawDetails(EditorContext& context)
{
    PanelHeader("Details", UiScale());
    if (!ImGui::BeginChild("##details_body", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, kScrollingPanelFlags))
    {
        ImGui::EndChild();
        return;
    }
    ModelImportedMaterialInfo& material = m_materials[static_cast<size_t>(m_selectedSlot)];
    MaterialGraphSelection& selection = m_slotUi[static_cast<size_t>(m_selectedSlot)].selection;
    bool changed = false;
    if (selection.nodes.size() == 1)
    {
        if (MaterialShaderNode* node = FindMaterialGraphNode(material.shaderGraph, *selection.nodes.begin()))
        {
            changed |= DrawNodeDetails(material, *node);
        }
    }
    else if (selection.nodes.size() > 1)
    {
        ImGui::TextDisabled("%zu nodes selected", selection.nodes.size());
        ImGui::TextWrapped("Drag them together on the graph, or Ctrl+C, Ctrl+D, Delete.");
    }
    else if (const MaterialShaderLink* link = FindMaterialGraphLink(material.shaderGraph, selection.link))
    {
        changed |= DrawLinkDetails(material, *link);
    }
    else
    {
        changed |= DrawMaterialDetails(material);
    }
    ImGui::EndChild();
    if (changed)
    {
        EndFrameEdits(context, true, false);
        BeginFrameEdits();
    }
}

bool MaterialEditorWindow::DrawMaterialDetails(ModelImportedMaterialInfo& material)
{
    const size_t slot = static_cast<size_t>(m_selectedSlot);
    ImGui::Spacing();
    ImGui::TextUnformatted(BuildMaterialSlotLabel(material, slot).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled(m_slotAsImported[slot] ? "(as imported)" : "(edited)");
    ImGui::TextDisabled("Slot %zu, %u parts, %s triangles", slot, m_slotSubmeshes.empty() ? 0u : m_slotSubmeshes[slot], FormatCount(m_slotTriangles.empty() ? 0 : m_slotTriangles[slot]).c_str());
    bool changed = false;
    if (ImGui::CollapsingHeader("Quick Edit", ImGuiTreeNodeFlags_DefaultOpen))
    {
        changed |= DrawQuickEdit(material, m_quickEditBrightness);
    }
    if (MaterialShaderNode* output = FindMaterialGraphOutputNode(material.shaderGraph))
    {
        changed |= DrawOutputProperties(output->pbr, LinkedInputs{&material.shaderGraph, output->id});
    }
    if (ImGui::CollapsingHeader("Resolved Material"))
    {
        DrawResolvedMaterial(material);
    }
    return changed;
}

bool MaterialEditorWindow::DrawNodeDetails(ModelImportedMaterialInfo& material, MaterialShaderNode& node)
{
    bool changed = false;
    const float scale = UiScale();
    ImGui::Spacing();
    const ImVec4 header = ImGui::ColorConvertU32ToFloat4(GetMaterialGraphHeaderColor(node.type));
    ImGui::TextColored(ImVec4(std::min(header.x * 1.8f, 1.0f), std::min(header.y * 1.8f, 1.0f), std::min(header.z * 1.8f, 1.0f), 1.0f), "%s", GetMaterialGraphNodeTypeLabel(node.type));
    ImGui::SameLine();
    ImGui::TextDisabled("node %u", node.id);
    if (m_nodeNameFor != node.id)
    {
        std::snprintf(m_nodeName, sizeof(m_nodeName), "%s", node.name.c_str());
        m_nodeNameFor = node.id;
    }
    if (BeginProperties("##node"))
    {
        PropertyLabel("Name");
        if (ImGui::InputText("##name", m_nodeName, sizeof(m_nodeName)))
        {
            node.name = m_nodeName;
            changed = true;
        }
        switch (node.type)
        {
        case MaterialShaderNodeType::Scalar:
            changed |= PropertyFloat("Value", &node.scalarValue, 0.0f, 8.0f, "%.3f");
            break;
        case MaterialShaderNodeType::Color:
            changed |= PropertyColor("Color", node.colorValue, 4, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
            break;
        case MaterialShaderNodeType::Blend:
            changed |= PropertyFloat("Default Factor", &node.scalarValue, 0.0f, 1.0f, "%.2f", "Used while nothing is linked into Factor.");
            break;
        default:
            break;
        }
        ImGui::EndTable();
    }
    if (node.type == MaterialShaderNodeType::Texture)
    {
        ImGui::SeparatorText("Texture");
        const ImTextureRef thumbnail = node.texturePath.empty() ? ImTextureRef() : Thumbnail(node.texturePath);
        const float side = std::min(ImGui::GetContentRegionAvail().x, 160.0f * scale);
        if (thumbnail._TexData != nullptr || thumbnail.GetTexID() != ImTextureID_Invalid)
        {
            ImGui::Image(thumbnail, ImVec2(side, side));
        }
        else
        {
            ImGui::Dummy(ImVec2(side, side * 0.25f));
            ImGui::SameLine();
            ImGui::TextDisabled(node.texturePath.empty() ? "No texture" : "Loading...");
        }
        ImGui::TextWrapped("%s", node.texturePath.empty() ? "No file chosen." : node.texturePath.c_str());
        if (ImGui::Button(ICON_PH_IMAGE "  Choose..."))
        {
            m_pickTextureNode = node.id;
            m_pickTextureRequested = true;
        }
        ImGui::SetItemTooltip("Pick an image file for this node (double-click the node does the same).");
        ImGui::SameLine();
        ImGui::BeginDisabled(node.texturePath.empty());
        if (ImGui::Button(ICON_PH_X "  Clear"))
        {
            node.texturePath.clear();
            changed = true;
        }
        ImGui::EndDisabled();
    }
    // What the node feeds, for finding it in a large graph.
    ImGui::SeparatorText("Links");
    bool any = false;
    for (const MaterialShaderLink& link : material.shaderGraph.links)
    {
        if (link.fromNodeId != node.id && link.toNodeId != node.id)
        {
            continue;
        }
        any = true;
        const MaterialShaderNode* from = FindMaterialGraphNode(material.shaderGraph, link.fromNodeId);
        const MaterialShaderNode* to = FindMaterialGraphNode(material.shaderGraph, link.toNodeId);
        const MaterialGraphPinDefinition* toPin = to != nullptr ? FindMaterialGraphPin(to->type, link.toSlot, true) : nullptr;
        ImGui::BulletText(
            "%s  %s  %s.%s",
            from != nullptr ? MaterialGraphNodeTitle(*from).c_str() : "?",
            ICON_PH_ARROW_FAT_RIGHT,
            to != nullptr ? MaterialGraphNodeTitle(*to).c_str() : "?",
            toPin != nullptr ? toPin->label : link.toSlot.c_str());
    }
    if (!any)
    {
        ImGui::TextDisabled("Not linked. Drag from a pin to link it.");
    }
    if (node.type == MaterialShaderNodeType::Output)
    {
        ImGui::Spacing();
        changed |= DrawOutputProperties(node.pbr, LinkedInputs{&material.shaderGraph, node.id});
    }
    return changed;
}

bool MaterialEditorWindow::DrawLinkDetails(ModelImportedMaterialInfo& material, const MaterialShaderLink& link)
{
    const MaterialShaderNode* from = FindMaterialGraphNode(material.shaderGraph, link.fromNodeId);
    const MaterialShaderNode* to = FindMaterialGraphNode(material.shaderGraph, link.toNodeId);
    const MaterialGraphPinDefinition* fromPin = from != nullptr ? FindMaterialGraphPin(from->type, link.fromSlot, false) : nullptr;
    const MaterialGraphPinDefinition* toPin = to != nullptr ? FindMaterialGraphPin(to->type, link.toSlot, true) : nullptr;
    ImGui::Spacing();
    ImGui::TextUnformatted("Link");
    if (BeginProperties("##link"))
    {
        PropertyLabel("From");
        ImGui::Text("%s . %s", from != nullptr ? MaterialGraphNodeTitle(*from).c_str() : "?", fromPin != nullptr ? fromPin->label : link.fromSlot.c_str());
        PropertyLabel("To");
        ImGui::Text("%s . %s", to != nullptr ? MaterialGraphNodeTitle(*to).c_str() : "?", toPin != nullptr ? toPin->label : link.toSlot.c_str());
        ImGui::EndTable();
    }
    if (ImGui::Button(ICON_PH_LINK_BREAK "  Delete Link"))
    {
        RemoveMaterialGraphLink(material.shaderGraph, link.id);
        m_slotUi[static_cast<size_t>(m_selectedSlot)].selection.link = 0;
        return true;
    }
    return false;
}

ImTextureRef MaterialEditorWindow::Thumbnail(const std::string& path)
{
    const std::string resolved = ResolveTexturePath(path);
    const std::shared_ptr<const MaterialPreviewTexture> texture = m_textures.Find(resolved);
    if (!texture)
    {
        m_textures.Request({resolved});
        return ImTextureRef();
    }
    std::unique_ptr<EditorUserTexture>& thumbnail = m_thumbnails[resolved];
    if (!thumbnail)
    {
        // The first level at most 128 on a side.
        const MaterialPreviewTexture::Level* level = &texture->levels.back();
        for (const MaterialPreviewTexture::Level& candidate : texture->levels)
        {
            if (candidate.width <= 128 && candidate.height <= 128)
            {
                level = &candidate;
                break;
            }
        }
        thumbnail = std::make_unique<EditorUserTexture>();
        thumbnail->Upload(level->width, level->height, level->rgba.data());
    }
    return thumbnail->Ref();
}

// ---- Graph ----------------------------------------------------------------------------------------

void MaterialEditorWindow::DrawGraph(EditorContext& context)
{
    const float scale = UiScale();
    ModelImportedMaterialInfo& material = m_materials[static_cast<size_t>(m_selectedSlot)];
    SlotUi& ui = m_slotUi[static_cast<size_t>(m_selectedSlot)];
    if (std::exchange(ui.arrangeOnShow, false))
    {
        ArrangeMaterialGraph(material.shaderGraph);
        ui.view.frameAll = true;
        // Laid out before any edit: the step an undo goes back to has it too.
        if (!m_undoStepOpen)
        {
            m_frameStart = CaptureSlot(m_selectedSlot);
        }
    }

    // The header: where the graph is, and its buttons.
    {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = ImGui::GetFrameHeight() + 6.0f * scale;
        ImGui::GetWindowDrawList()->AddRectFilled(min, ImVec2(min.x + width, min.y + height), kPanelHeader);
        ImGui::SetCursorScreenPos(ImVec2(min.x + 8.0f * scale, min.y + 3.0f * scale));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kPanelHeaderText), "Graph  %s  %s", ICON_PH_CARET_RIGHT, BuildMaterialSlotLabel(material, static_cast<size_t>(m_selectedSlot)).c_str());
        if (m_slotAsImported[static_cast<size_t>(m_selectedSlot)])
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(as imported)");
        }
        const float buttons = ImGui::CalcTextSize(ICON_PH_HOUSE).x + ImGui::CalcTextSize(ICON_PH_TREE_STRUCTURE).x + 4.0f * ImGui::GetStyle().FramePadding.x + 14.0f * scale;
        ImGui::SameLine(std::max(width - buttons, ImGui::GetCursorPosX()));
        if (ImGui::Button(ICON_PH_HOUSE))
        {
            m_graph.FrameSelection(MaterialGraphSelection{});
        }
        ImGui::SetItemTooltip("Show the whole graph (Home)");
        ImGui::SameLine(0.0f, 2.0f * scale);
        bool arranged = false;
        if (ImGui::Button(ICON_PH_TREE_STRUCTURE))
        {
            ArrangeMaterialGraph(material.shaderGraph);
            m_graph.FrameSelection(MaterialGraphSelection{});
            arranged = true;
        }
        ImGui::SetItemTooltip("Arrange the nodes in columns, from the Output leftward");
        ImGui::SetCursorScreenPos(ImVec2(min.x, min.y + height));
        if (arranged)
        {
            EndFrameEdits(context, true, false);
            BeginFrameEdits();
        }
    }

    MaterialGraphCanvas::Context canvasContext;
    canvasContext.uiScale = scale;
    canvasContext.thumbnail = [this](const std::string& path)
    {
        return Thumbnail(path);
    };
    std::string canvasStatus;
    canvasContext.statusMessage = &canvasStatus;
    const MaterialGraphCanvas::Result result = m_graph.Draw(material.shaderGraph, ui.view, ui.selection, canvasContext);
    if (!canvasStatus.empty())
    {
        SetStatus(canvasStatus);
    }
    // Nodes dragged in from the palette.
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kNodePayload))
        {
            const MaterialShaderNodeType type = *static_cast<const MaterialShaderNodeType*>(payload->Data);
            if (type != MaterialShaderNodeType::Output || !MaterialGraphHasOutputNode(material.shaderGraph))
            {
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                const ImVec2 canvasMin = ImGui::GetItemRectMin();
                MaterialGraphNodePosition at{
                    ui.view.origin.x + (mouse.x - canvasMin.x) / (scale * ui.view.zoom),
                    ui.view.origin.y + (mouse.y - canvasMin.y) / (scale * ui.view.zoom)};
                MaterialShaderNode* node = AddMaterialGraphNode(material.shaderGraph, type, at);
                ui.selection.nodes = {node->id};
                ui.selection.link = 0;
                EndFrameEdits(context, true, false);
                BeginFrameEdits();
            }
        }
        ImGui::EndDragDropTarget();
    }
    m_graphFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    if (result.pickTextureForNode.has_value())
    {
        m_pickTextureNode = *result.pickTextureForNode;
        m_pickTextureRequested = true;
    }
    if (result.changed)
    {
        EndFrameEdits(context, true, result.dragging);
        BeginFrameEdits();
    }
    else
    {
        EndFrameEdits(context, false, result.dragging);
    }
}

// ---- Slots and palette ----------------------------------------------------------------------------

void MaterialEditorWindow::DrawSlots(EditorContext& context)
{
    const float scale = UiScale();
    const std::string title = "Material Slots (" + std::to_string(m_materials.size()) + ")";
    PanelHeader(title.c_str(), scale);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f * scale);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 4.0f * scale);
    ImGui::InputTextWithHint("##slot_filter", ICON_PH_MAGNIFYING_GLASS "  Search slots", m_slotFilter, sizeof(m_slotFilter));
    if (!ImGui::BeginChild("##slot_list", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, kScrollingPanelFlags))
    {
        ImGui::EndChild();
        return;
    }
    const float swatch = ImGui::GetTextLineHeight();
    std::optional<int> clicked;
    for (size_t slot = 0; slot < m_materials.size(); ++slot)
    {
        const ModelImportedMaterialInfo& material = m_materials[slot];
        const std::string label = BuildMaterialSlotLabel(material, slot);
        if (!FuzzyMatchScore(m_slotFilter, label).has_value())
        {
            continue;
        }
        ImGui::PushID(static_cast<int>(slot));
        const bool selected = static_cast<int>(slot) == m_selectedSlot;
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##slot", selected, ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, swatch + 4.0f * scale)))
        {
            clicked = static_cast<int>(slot);
        }
        if (ImGui::BeginPopupContextItem("##slot_menu"))
        {
            if (ImGui::MenuItem("Show Imported Material", nullptr, m_slotAsImported[slot]))
            {
                SelectSlot(static_cast<int>(slot));
                SetSlotAsImported(context, static_cast<int>(slot), true);
            }
            if (ImGui::MenuItem("Show Edit", nullptr, !m_slotAsImported[slot], m_stashedEdits[slot].has_value() || !m_slotAsImported[slot]))
            {
                SelectSlot(static_cast<int>(slot));
                SetSlotAsImported(context, static_cast<int>(slot), false);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Copy Name"))
            {
                ImGui::SetClipboardText(label.c_str());
            }
            ImGui::EndPopup();
        }
        ImGui::SetItemTooltip("%s\n%u parts, %s triangles%s", label.c_str(), m_slotSubmeshes.empty() ? 0u : m_slotSubmeshes[slot],
                              FormatCount(m_slotTriangles.empty() ? 0 : m_slotTriangles[slot]).c_str(),
                              m_editedSlots.contains(static_cast<uint32_t>(slot)) ? "\nEdited, not saved" : "");
        // The row: the base colour's swatch, the name, and its state.
        ImDrawList& drawList = *ImGui::GetWindowDrawList();
        const ImVec2 swatchMin(rowMin.x + 4.0f * scale, rowMin.y + 2.0f * scale);
        const ImVec4 colour(
            std::pow(std::clamp(material.pbr.baseColorFactor[0], 0.0f, 1.0f), 1.0f / kGamma),
            std::pow(std::clamp(material.pbr.baseColorFactor[1], 0.0f, 1.0f), 1.0f / kGamma),
            std::pow(std::clamp(material.pbr.baseColorFactor[2], 0.0f, 1.0f), 1.0f / kGamma),
            1.0f);
        drawList.AddRectFilled(swatchMin, ImVec2(swatchMin.x + swatch, swatchMin.y + swatch), ImGui::ColorConvertFloat4ToU32(colour), 3.0f * scale);
        if (!material.baseColorTexturePath.empty())
        {
            // A map multiplies the colour: a corner marks it.
            drawList.AddTriangleFilled(
                ImVec2(swatchMin.x + swatch * 0.55f, swatchMin.y + swatch), ImVec2(swatchMin.x + swatch, swatchMin.y + swatch * 0.55f),
                ImVec2(swatchMin.x + swatch, swatchMin.y + swatch), IM_COL32(90, 160, 245, 255));
        }
        drawList.AddRect(swatchMin, ImVec2(swatchMin.x + swatch, swatchMin.y + swatch), IM_COL32(0, 0, 0, 160), 3.0f * scale);
        const bool edited = m_editedSlots.contains(static_cast<uint32_t>(slot));
        const std::string text = label + (edited ? " *" : "");
        drawList.AddText(ImVec2(swatchMin.x + swatch + 6.0f * scale, rowMin.y + 2.0f * scale), ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
        if (!m_slotAsImported[slot])
        {
            const char* badge = "edit";
            const float badgeWidth = ImGui::CalcTextSize(badge).x;
            const float right = rowMin.x + ImGui::GetContentRegionAvail().x - 4.0f * scale;
            drawList.AddText(ImVec2(right - badgeWidth, rowMin.y + 2.0f * scale), ImGui::GetColorU32(ImGuiCol_TextDisabled), badge);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (clicked.has_value())
    {
        SelectSlot(*clicked);
    }
}

void MaterialEditorWindow::DrawPalette(EditorContext& context)
{
    const float scale = UiScale();
    PanelHeader("Palette", scale);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f * scale);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 4.0f * scale);
    ImGui::InputTextWithHint("##palette_filter", ICON_PH_MAGNIFYING_GLASS "  Search nodes", m_paletteFilter, sizeof(m_paletteFilter));
    if (!ImGui::BeginChild("##palette_list", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, kScrollingPanelFlags))
    {
        ImGui::EndChild();
        return;
    }
    ModelImportedMaterialInfo& material = m_materials[static_cast<size_t>(m_selectedSlot)];
    const char* category = nullptr;
    for (const MaterialGraphNodeKind& kind : MaterialGraphNodeKinds())
    {
        if (!FuzzyMatchScore(m_paletteFilter, kind.name).has_value())
        {
            continue;
        }
        if (category == nullptr || std::string_view(category) != kind.category)
        {
            category = kind.category;
            ImGui::SeparatorText(category);
        }
        const bool unavailable = kind.type == MaterialShaderNodeType::Output && MaterialGraphHasOutputNode(material.shaderGraph);
        ImGui::PushID(kind.name);
        ImGui::BeginDisabled(unavailable);
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float swatch = ImGui::GetTextLineHeight() * 0.7f;
        ImGui::Selectable("##kind", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, ImGui::GetTextLineHeight() + 2.0f * scale));
        const bool doubleClicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        if (!unavailable && ImGui::BeginDragDropSource())
        {
            const MaterialShaderNodeType type = kind.type;
            ImGui::SetDragDropPayload(kNodePayload, &type, sizeof(type));
            ImGui::Text("%s", kind.name);
            ImGui::EndDragDropSource();
        }
        ImGui::SetItemTooltip("%s\nDrag onto the graph, or double-click.", kind.description);
        ImDrawList& drawList = *ImGui::GetWindowDrawList();
        const ImVec2 swatchMin(rowMin.x + 6.0f * scale, rowMin.y + (ImGui::GetTextLineHeight() - swatch) * 0.5f + 1.0f * scale);
        drawList.AddRectFilled(swatchMin, ImVec2(swatchMin.x + swatch, swatchMin.y + swatch), GetMaterialGraphHeaderColor(kind.type), 2.0f * scale);
        drawList.AddText(ImVec2(swatchMin.x + swatch + 8.0f * scale, rowMin.y + 1.0f * scale), ImGui::GetColorU32(unavailable ? ImGuiCol_TextDisabled : ImGuiCol_Text), kind.name);
        ImGui::EndDisabled();
        if (doubleClicked && !unavailable)
        {
            SlotUi& ui = m_slotUi[static_cast<size_t>(m_selectedSlot)];
            // In the middle of the graph's view.
            MaterialGraphNodePosition at{ui.view.origin.x + 200.0f / ui.view.zoom, ui.view.origin.y + 150.0f / ui.view.zoom};
            MaterialShaderNode* node = AddMaterialGraphNode(material.shaderGraph, kind.type, at);
            ui.selection.nodes = {node->id};
            ui.selection.link = 0;
            EndFrameEdits(context, true, false);
            BeginFrameEdits();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void MaterialEditorWindow::DrawStatusBar()
{
    const float scale = UiScale();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetTextLineHeight() + 6.0f * scale;
    ImDrawList& drawList = *ImGui::GetWindowDrawList();
    drawList.AddRectFilled(min, ImVec2(min.x + width, min.y + height), kPanelHeader);
    const double age = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_statusTime).count();
    const bool recent = !m_status.empty() && age < kStatusSeconds;
    const std::string left = recent ? m_status
                                    : std::string("Right-click the graph to add a node  |  drag from a pin to link  |  right drag pans, wheel zooms  |  Ctrl+Z undo");
    drawList.AddText(ImVec2(min.x + 8.0f * scale, min.y + 3.0f * scale), ImGui::GetColorU32(recent ? ImGuiCol_Text : ImGuiCol_TextDisabled), left.c_str());
    const ModelImportedMaterialInfo& material = m_materials[static_cast<size_t>(m_selectedSlot)];
    const std::string right = "Slot " + std::to_string(m_selectedSlot + 1) + " of " + std::to_string(m_materials.size()) + "   " +
                              std::to_string(material.shaderGraph.nodes.size()) + " nodes   " + std::to_string(material.shaderGraph.links.size()) + " links";
    const float rightWidth = ImGui::CalcTextSize(right.c_str()).x;
    drawList.AddText(ImVec2(min.x + width - rightWidth - 8.0f * scale, min.y + 3.0f * scale), ImGui::GetColorU32(ImGuiCol_TextDisabled), right.c_str());
    ImGui::Dummy(ImVec2(width, height));
}
}
