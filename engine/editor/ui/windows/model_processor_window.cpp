#include "model_processor_window.h"

#include <engine/asset/asset_paths.h>
#include <engine/editor/editor_ui.h>
#include <engine/editor/ui/editor_material_graph.h>
#include <engine/editor/ui/editor_model_preview.h>
#include <engine/editor/ui/editor_ui_internal.h>
#include <engine/editor/ui/framework/editor_window_manager.h>

#include <engine/asset/material_graph_runtime.h>
#include <engine/asset/material_definition.h>
#include <engine/asset/model_loader.h>

#include <engine/logic/editor_world.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <string_view>
#include <vector>

namespace me
{

namespace
{
std::string BuildMaterialSlotLabel(const ModelImportedMaterialInfo& material, size_t materialIndex)
{
    return material.name.empty()
               ? ("Material " + std::to_string(materialIndex))
               : material.name;
}

// The model's materials as the editor edits them, each with a compiled shader graph; one default
// material for a model without any.
std::vector<ModelImportedMaterialInfo> BuildEditableMaterials(const LoadedModelData& loadedModel)
{
    std::vector<ModelImportedMaterialInfo> materials;
    materials.reserve(loadedModel.materials.size());
    for (const ModelMaterialData& material : loadedModel.materials)
    {
        ModelImportedMaterialInfo importedMaterial = BuildImportedMaterialInfo(material);
        EnsureMaterialShaderGraph(importedMaterial.name, std::nullopt, importedMaterial);
        CompileMaterialShaderGraph(importedMaterial);
        materials.push_back(std::move(importedMaterial));
    }

    if (materials.empty())
    {
        materials.push_back(ModelImportedMaterialInfo{});
    }
    return materials;
}

// The canvas grid, scrolling and scaling with the view.
void DrawMaterialGraphGrid(
    ImDrawList* drawList,
    const ImVec2& canvasOrigin,
    const ImVec2& canvasMax,
    const MaterialGraphNodePosition& viewOrigin,
    float zoom,
    float gridStep)
{
    const float gridOffsetX =
        std::fmod(-(viewOrigin.x * zoom), gridStep);
    for (float x = gridOffsetX; x < canvasMax.x - canvasOrigin.x; x += gridStep)
    {
        drawList->AddLine(
            ImVec2(canvasOrigin.x + x, canvasOrigin.y),
            ImVec2(canvasOrigin.x + x, canvasMax.y),
            IM_COL32(44, 54, 70, 90),
            1.0f);
    }
    const float gridOffsetY =
        std::fmod(-(viewOrigin.y * zoom), gridStep);
    for (float y = gridOffsetY; y < canvasMax.y - canvasOrigin.y; y += gridStep)
    {
        drawList->AddLine(
            ImVec2(canvasOrigin.x, canvasOrigin.y + y),
            ImVec2(canvasMax.x, canvasOrigin.y + y),
            IM_COL32(44, 54, 70, 90),
            1.0f);
    }
}

// What the material resolved to: its textures and every factor.
void DrawResolvedMaterial(const ModelImportedMaterialInfo& material)
{
    ImGui::SeparatorText("Resolved Material");
    const MaterialTextureBlendGraph& blendGraph = material.blendGraph;
    DrawPrimaryMaterialTextureRows(material);
    ImGui::Text(
        "Metallic %.2f  Roughness %.2f  Normal %.2f  AO %.2f  Emissive %.2f  Opacity %.2f",
        material.pbr.metallicFactor,
        material.pbr.roughnessFactor,
        material.pbr.normalScale,
        material.pbr.occlusionStrength,
        material.pbr.emissiveIntensity,
        material.pbr.opacity);
    ImGui::Text(
        "Clearcoat %.2f  Clearcoat Roughness %.2f",
        material.pbr.clearcoatFactor,
        material.pbr.clearcoatRoughnessFactor);
    ImGui::Text(
        "Sheen Color %.2f %.2f %.2f  Sheen Roughness %.2f",
        material.pbr.sheenColorFactor[0],
        material.pbr.sheenColorFactor[1],
        material.pbr.sheenColorFactor[2],
        material.pbr.sheenRoughnessFactor);
    ImGui::Text(
        "Anisotropy %.2f  Rotation %.1f deg",
        material.pbr.anisotropyStrength,
        material.pbr.anisotropyRotation * (180.0f / 3.14159265f));
    ImGui::Text(
        "IOR %.3f  Specular %.2f  Specular Color %.2f %.2f %.2f",
        material.pbr.ior,
        material.pbr.specularFactor,
        material.pbr.specularColorFactor[0],
        material.pbr.specularColorFactor[1],
        material.pbr.specularColorFactor[2]);
    ImGui::Text(
        "Iridescence %.2f  IOR %.2f  Thickness %.0f-%.0f nm",
        material.pbr.iridescenceFactor,
        material.pbr.iridescenceIor,
        material.pbr.iridescenceThicknessMinimum,
        material.pbr.iridescenceThicknessMaximum);
    ImGui::Text(
        "Transmission %.2f  Thickness %.3f  Attenuation %.3f m (%.2f, %.2f, %.2f)",
        material.pbr.transmissionFactor,
        material.pbr.thicknessFactor,
        material.pbr.attenuationDistance,
        material.pbr.attenuationColor[0],
        material.pbr.attenuationColor[1],
        material.pbr.attenuationColor[2]);
    ImGui::Text(
        "Dispersion %.2f  Diffuse Transmission %.2f (%.2f, %.2f, %.2f)",
        material.pbr.dispersion,
        material.pbr.diffuseTransmissionFactor,
        material.pbr.diffuseTransmissionColor[0],
        material.pbr.diffuseTransmissionColor[1],
        material.pbr.diffuseTransmissionColor[2]);
    if (material.pbr.volumeScatter)
    {
        ImGui::Text(
            "Volume Scatter (%.2f, %.2f, %.2f)  Anisotropy %.2f",
            material.pbr.multiscatterColor[0],
            material.pbr.multiscatterColor[1],
            material.pbr.multiscatterColor[2],
            material.pbr.scatterAnisotropy);
    }
    ImGui::Text("Alpha Mode: %s", ToString(material.pbr.alphaMode));
    if (material.pbr.alphaMode == MaterialAlphaMode::Mask)
    {
        ImGui::Text("Alpha Cutoff: %.2f", material.pbr.alphaCutoff);
    }
    if (HasSecondaryMaterialLayer(blendGraph))
    {
        ImGui::Separator();
        DrawSecondaryMaterialTextureRows(blendGraph);
    }
}

// The Color or Scalar node linked into one of the Output node's inputs, which the compile takes
// over the Output's own factor; nullptr when the input is the factor itself.
MaterialShaderNode* FindLinkedOutputInput(
    MaterialShaderGraph& graph,
    uint32_t outputId,
    std::string_view slot,
    MaterialShaderNodeType type)
{
    for (const MaterialShaderLink& link : graph.links)
    {
        if (link.toNodeId == outputId && link.toSlot == slot)
        {
            MaterialShaderNode* node = FindMaterialGraphNode(graph, link.fromNodeId);
            return node != nullptr && node->type == type ? node : nullptr;
        }
    }
    return nullptr;
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

// The factors a colour change needs, without the graph. They are written where the compile reads
// them: the Output node's own factors, or the Color or Scalar node linked into them.
bool DrawMaterialQuickEdit(ModelImportedMaterialInfo& material, float& brightness)
{
    ImGui::SeparatorText("Quick Edit");
    const auto outputIterator = std::find_if(
        material.shaderGraph.nodes.begin(),
        material.shaderGraph.nodes.end(),
        [](const MaterialShaderNode& node)
        {
            return node.type == MaterialShaderNodeType::Output;
        });
    if (outputIterator == material.shaderGraph.nodes.end())
    {
        ImGui::TextDisabled("The material graph has no Output node.");
        return false;
    }
    MaterialShaderNode& output = *outputIterator;
    bool changed = false;

    // The factor is linear. It is edited in gamma, as the picker shows colours, and split into a
    // colour in [0, 1] and a brightness, so a factor above 1 (a kn5 paint's diffuse gain) stays
    // editable. The brightness is kept between frames while the factor fits under it.
    MaterialShaderNode* colorNode =
        FindLinkedOutputInput(material.shaderGraph, output.id, "base_factor", MaterialShaderNodeType::Color);
    float* factor = colorNode != nullptr ? colorNode->colorValue : output.pbr.baseColorFactor;
    constexpr float kGamma = 2.2f;
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
    bool colorChanged = ImGui::ColorEdit3("Base Color", color, ImGuiColorEditFlags_PickerHueWheel);
    colorChanged |= DragFloatInRange("Brightness", &brightness, 0.05f, 4.0f, "%.2f");
    if (colorChanged)
    {
        for (size_t channel = 0; channel < 3; ++channel)
        {
            factor[channel] = std::pow(std::clamp(color[channel], 0.0f, 1.0f) * brightness, kGamma);
        }
        changed = true;
    }
    if (!material.baseColorTexturePath.empty())
    {
        ImGui::TextDisabled(
            "Multiplies the base map %s",
            std::filesystem::path(material.baseColorTexturePath).filename().string().c_str());
    }

    const auto scalarInput = [&](std::string_view slot, float& outputFactor) -> float&
    {
        MaterialShaderNode* node =
            FindLinkedOutputInput(material.shaderGraph, output.id, slot, MaterialShaderNodeType::Scalar);
        return node != nullptr ? node->scalarValue : outputFactor;
    };
    changed |= DragFloatInRange("Metallic", &scalarInput("metallic_factor", output.pbr.metallicFactor), 0.0f, 1.0f, "%.2f");
    changed |= DragFloatInRange("Roughness", &scalarInput("roughness_factor", output.pbr.roughnessFactor), 0.0f, 1.0f, "%.2f");
    changed |= DragFloatInRange("Clearcoat", &output.pbr.clearcoatFactor, 0.0f, 1.0f, "%.2f");
    changed |= DragFloatInRange("Clearcoat Roughness", &output.pbr.clearcoatRoughnessFactor, 0.0f, 1.0f, "%.2f");
    return changed;
}
}

ModelProcessorWindow::ModelProcessorWindow()
    : EditorWindow("model_preview", "Model Preview")
{
}

void ModelProcessorWindow::OpenModel(EditorContext& context, const std::string& modelPath, bool preselectPaint)
{
    RevertScenePreview(context);
    LoadModel(modelPath, preselectPaint);
    context.windows.Open(*this);
}

void ModelProcessorWindow::LoadModel(const std::string& modelPath, bool preselectPaint)
{
    const std::filesystem::path normalizedPath = NormalizeFilesystemPath(modelPath);
    Reset();
    m_modelPath = normalizedPath.string();
    m_displayName = normalizedPath.filename().string();
    m_quickEditBrightness = 0.0f;
    m_modelPreview = PreviewCamera{};
    m_modelPreview.autoFramePending = true;

    try
    {
        LoadedModelData loadedModel = ModelLoader::LoadModelAsImported(m_modelPath);
        m_importedMaterials = BuildEditableMaterials(loadedModel);
        ModelLoader::ApplyMaterialDefinitions(m_modelPath, loadedModel);
        m_materials = BuildEditableMaterials(loadedModel);
        m_loadedModel = std::move(loadedModel);
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
        if (preselectPaint)
        {
            m_selectedMaterialIndex = FindPaintSlot(m_materials);
        }
    }
    catch (const std::exception& error)
    {
        m_statusMessage = error.what();
    }
}

void ModelProcessorWindow::Reset()
{
    ResetMaterialShadedPreviewCache("ModelDraftPreviewCanvas");
    m_modelPath.clear();
    m_displayName.clear();
    m_statusMessage.clear();
    m_loadedModel = LoadedModelData{};
    m_materials.clear();
    m_importedMaterials.clear();
    m_slotAsImported.clear();
    m_stashedEdits.clear();
    m_selectedMaterialIndex = 0;
    m_selectedUvSubmeshIndex = 0;
    m_dirty = false;
    m_editedSlots.clear();
    m_scenePreviewed = false;
    m_materialGraph = MaterialGraphCanvas{};
}

void ModelProcessorWindow::OnAssetRenamed(const std::string& oldPath, const std::string& newPath)
{
    if (const std::optional<std::filesystem::path> rebased = AssetPaths::Rebase(m_modelPath, oldPath, newPath))
    {
        m_modelPath = rebased->string();
        m_displayName = rebased->filename().string();
    }
}

void ModelProcessorWindow::RevertScenePreview(EditorContext& context)
{
    if (m_scenePreviewed)
    {
        context.result.actions.revertImportedModelMaterials = m_modelPath;
        m_scenePreviewed = false;
    }
}

void ModelProcessorWindow::Tick(EditorContext& context)
{
    static_cast<void>(context);
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
    if (m_modelPath.empty() || !std::filesystem::exists(modelPath, errorCode) || errorCode ||
        !IsSupportedModelAssetPath(modelPath))
    {
        // Nothing to read back from disk: the scene's copies went with the file.
        m_scenePreviewed = false;
        Close();
    }
}

void ModelProcessorWindow::OnClose(EditorContext& context)
{
    RevertScenePreview(context);
    Reset();
}

void ModelProcessorWindow::PreBegin(EditorContext& context)
{
    static_cast<void>(context);
    ImGui::SetNextWindowSize(ImVec2(560.0f * UiScale(), 560.0f * UiScale()), ImGuiCond_FirstUseEver);
}

void ModelProcessorWindow::OnGui(EditorContext& context)
{
    IEditorWorld& scene = context.scene;
    EditorUiFrameResult& result = context.result;
    bool requestReload = false;
    ImGui::TextWrapped("Model: %s", m_displayName.empty() ? "<unknown>" : m_displayName.c_str());
    ImGui::TextWrapped("Asset Path: %s", m_modelPath.c_str());
    ImGui::TextWrapped(
        "Scene Target: %s",
        scene.HasSelection() ? scene.GetSelectedTag().name.c_str() : "<no entity selected>");

    if (!m_statusMessage.empty())
    {
        ImGui::Spacing();
        ImGui::TextWrapped("Status: %s", m_statusMessage.c_str());
    }

    ImGui::BeginDisabled(!scene.HasSelection());
    if (ImGui::Button("Load Into Selected Entity", ImVec2(220.0f * UiScale(), 0.0f)))
    {
        result.actions.selectedModelPath = m_modelPath;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reload From Disk"))
    {
        requestReload = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(140.0f * UiScale(), 0.0f)))
    {
        Close();
    }

    if (!m_loadedModel.submeshes.empty())
    {
        uint32_t previewTriangleCount = 0;
        for (const ModelSubmeshData& submesh : m_loadedModel.submeshes)
        {
            previewTriangleCount += static_cast<uint32_t>(submesh.mesh.indices.size() / 3u);
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Live Draft Preview");
        DrawMaterialShadedPreview(
            m_loadedModel,
            m_materials,
            m_materials.empty() ? -1 : m_selectedMaterialIndex,
            m_modelPreview.yaw,
            m_modelPreview.pitch,
            m_modelPreview.distance,
            m_modelPreview.autoFramePending,
            UiScale(),
            "ModelDraftPreviewCanvas",
            "Approximate PBR draft preview");
        ImGui::Text(
            "Submeshes: %u  Triangles: %u",
            static_cast<unsigned int>(m_loadedModel.submeshes.size()),
            static_cast<unsigned int>(previewTriangleCount));

        ImGui::Spacing();
        ImGui::SeparatorText("UV Preview");
        DrawModelUvPreview(
            m_loadedModel,
            m_selectedUvSubmeshIndex,
            UiScale());
    }

    if (m_materials.empty())
    {
        ImGui::Spacing();
        ImGui::TextDisabled("No material slots are available right now.");
    }
    else
    {
        m_selectedMaterialIndex = std::clamp(
            m_selectedMaterialIndex,
            0,
            static_cast<int>(m_materials.size()) - 1);

        const std::string currentSlotLabel = BuildMaterialSlotLabel(
            m_materials[static_cast<size_t>(m_selectedMaterialIndex)],
            static_cast<size_t>(m_selectedMaterialIndex));

        if (ImGui::BeginCombo("Material Slot", currentSlotLabel.c_str()))
        {
            for (size_t materialIndex = 0; materialIndex < m_materials.size(); ++materialIndex)
            {
                const bool isSelected = static_cast<int>(materialIndex) == m_selectedMaterialIndex;
                const std::string label =
                    BuildMaterialSlotLabel(m_materials[materialIndex], materialIndex);
                if (ImGui::Selectable(label.c_str(), isSelected))
                {
                    m_selectedMaterialIndex = static_cast<int>(materialIndex);
                    m_quickEditBrightness = 0.0f;
                }
                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        const size_t selectedMaterialIndex = static_cast<size_t>(m_selectedMaterialIndex);
        ModelImportedMaterialInfo& selectedMaterial = m_materials[selectedMaterialIndex];
        ImGui::Text("Draft State: %s", m_dirty ? "Modified" : "Clean");
        if (ImGui::Checkbox("Live Preview in Scene", &m_livePreview))
        {
            if (!m_livePreview)
            {
                RevertScenePreview(context);
            }
            else if (!m_editedSlots.empty())
            {
                EditorUiActions::ImportedModelMaterialPreview preview{m_modelPath, {}};
                for (const uint32_t slot : m_editedSlots)
                {
                    preview.materials.emplace_back(slot, m_materials[slot]);
                }
                result.actions.previewImportedModelMaterial = std::move(preview);
                m_scenePreviewed = true;
            }
        }
        ImGui::SetItemTooltip("Show edits on the scene's copies of this model as they are made. Save writes them to disk; closing without saving drops them.");

        // The slot as its import made it, or the edit: switching back and forth compares them, and
        // Save keeps the one shown.
        bool switched = false;
        bool asImported = m_slotAsImported[selectedMaterialIndex];
        std::optional<ModelImportedMaterialInfo>& stashedEdit = m_stashedEdits[selectedMaterialIndex];
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Material");
        ImGui::SameLine();
        if (ImGui::RadioButton("As Imported", asImported) && !asImported)
        {
            stashedEdit = selectedMaterial;
            selectedMaterial = m_importedMaterials[selectedMaterialIndex];
            asImported = true;
            switched = true;
        }
        ImGui::SetItemTooltip("The material exactly as the import made it (for a car, the livery's own paint). Save removes the edit saved for this slot.");
        ImGui::SameLine();
        ImGui::BeginDisabled(asImported && !stashedEdit.has_value());
        if (ImGui::RadioButton("Edited", !asImported) && asImported)
        {
            selectedMaterial = *stashedEdit;
            stashedEdit.reset();
            asImported = false;
            switched = true;
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Your edit of this slot. Changing anything below makes the slot an edit.");
        if (switched)
        {
            m_quickEditBrightness = 0.0f;
            m_materialGraph.selectedNodeId = 0;
            m_materialGraph.selectedLinkId = 0;
        }

        bool materialChanged = DrawMaterialQuickEdit(selectedMaterial, m_quickEditBrightness);
        materialChanged |= DrawMaterialGraphEditor(selectedMaterial, selectedMaterialIndex);
        if (materialChanged)
        {
            // An edit starts from what is shown, the imported material included.
            asImported = false;
            stashedEdit.reset();
        }
        m_slotAsImported[selectedMaterialIndex] = asImported;
        materialChanged |= switched;
        if (materialChanged)
        {
            const MaterialGraphCompileResult compileResult =
                CompileMaterialShaderGraph(selectedMaterial);
            m_dirty = true;
            m_editedSlots.insert(static_cast<uint32_t>(selectedMaterialIndex));
            if (!compileResult.message.empty())
            {
                m_statusMessage = compileResult.message;
            }
            if (m_livePreview)
            {
                result.actions.previewImportedModelMaterial = EditorUiActions::ImportedModelMaterialPreview{
                    m_modelPath,
                    {{static_cast<uint32_t>(selectedMaterialIndex), selectedMaterial}}};
                m_scenePreviewed = true;
            }
        }

        DrawResolvedMaterial(selectedMaterial);

        ImGui::Spacing();
        ImGui::BeginDisabled(!m_dirty);
        if (ImGui::Button("Save Material Graph", ImVec2(220.0f * UiScale(), 0.0f)))
        {
            EditorUiActions::ImportedModelMaterialsUpdate update{m_modelPath, m_materials, {}, {}};
            for (const uint32_t slot : m_editedSlots)
            {
                (m_slotAsImported[slot] ? update.restoredIndices : update.indices).push_back(slot);
            }
            result.actions.updatedImportedModelMaterials = std::move(update);
            m_dirty = false;
            m_editedSlots.clear();
            m_scenePreviewed = false;
            m_statusMessage = "Saved material graph for slot: " + currentSlotLabel;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(140.0f * UiScale(), 0.0f)))
        {
            Close();
        }
    }

    // The window closes through OnClose; a reload drops the previewed edits and starts over.
    if (requestReload && IsOpen())
    {
        const std::string modelPath = m_modelPath;
        RevertScenePreview(context);
        LoadModel(modelPath, false);
    }
}

bool ModelProcessorWindow::DrawMaterialGraphEditor(ModelImportedMaterialInfo& material, size_t materialIndex)
{
    bool changed = false;
    EnsureMaterialShaderGraph(material.name, std::nullopt, material);
    if (m_materialGraph.selectedNodeId != 0 &&
        FindMaterialGraphNode(material.shaderGraph, m_materialGraph.selectedNodeId) == nullptr)
    {
        m_materialGraph.selectedNodeId = 0;
    }
    if (m_materialGraph.nodeResizeActive &&
        FindMaterialGraphNode(material.shaderGraph, m_materialGraph.resizeNodeId) == nullptr)
    {
        m_materialGraph.CancelResize();
    }
    if (m_materialGraph.selectedLinkId != 0 &&
        FindMaterialGraphLink(material.shaderGraph, m_materialGraph.selectedLinkId) == nullptr)
    {
        m_materialGraph.selectedLinkId = 0;
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Shader Node Graph");
    ImGui::TextWrapped("Left click selects nodes and links. Selected nodes can be moved by dragging empty space, resized from highlighted edges, and edited from the right-click card menu. Middle mouse pans the canvas, and the wheel zooms around the cursor.");

    const MaterialShaderNode* selectedGraphNodeForActions =
        FindMaterialGraphNode(material.shaderGraph, m_materialGraph.selectedNodeId);
    const MaterialShaderLink* selectedGraphLinkForActions =
        FindMaterialGraphLink(material.shaderGraph, m_materialGraph.selectedLinkId);

    if (ImGui::Button("Add Node", ImVec2(140.0f * UiScale(), 0.0f)))
    {
        m_materialGraph.openAddNodePopup = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(
        selectedGraphNodeForActions == nullptr ||
        selectedGraphNodeForActions->type == MaterialShaderNodeType::Output);
    if (ImGui::Button("Delete Selected Node", ImVec2(190.0f * UiScale(), 0.0f)))
    {
        RemoveMaterialGraphNode(material.shaderGraph, m_materialGraph.selectedNodeId);
        if (m_materialGraph.nodeResizeActive &&
            m_materialGraph.resizeNodeId == m_materialGraph.selectedNodeId)
        {
            m_materialGraph.CancelResize();
        }
        if (m_materialGraph.linkDragActive &&
            m_materialGraph.linkDragFromNodeId == m_materialGraph.selectedNodeId)
        {
            m_materialGraph.CancelLinkDrag();
        }
        m_materialGraph.selectedNodeId = 0;
        m_materialGraph.selectedLinkId = 0;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(selectedGraphLinkForActions == nullptr);
    if (ImGui::Button("Delete Selected Link", ImVec2(180.0f * UiScale(), 0.0f)))
    {
        RemoveMaterialGraphLink(material.shaderGraph, m_materialGraph.selectedLinkId);
        m_materialGraph.selectedLinkId = 0;
        changed = true;
    }
    ImGui::EndDisabled();
    if (m_materialGraph.linkDragActive)
    {
        ImGui::SameLine();
        ImGui::TextDisabled(
            "Linking: %u.%s",
            static_cast<unsigned int>(m_materialGraph.linkDragFromNodeId),
            m_materialGraph.linkDragFromSlot.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel Link", ImVec2(140.0f * UiScale(), 0.0f)))
        {
            m_materialGraph.CancelLinkDrag();
        }
    }

    const MaterialShaderNode* selectedGraphNode =
        FindMaterialGraphNode(material.shaderGraph, m_materialGraph.selectedNodeId);
    const MaterialShaderLink* selectedGraphLink =
        FindMaterialGraphLink(material.shaderGraph, m_materialGraph.selectedLinkId);
    if (selectedGraphNode != nullptr)
    {
        ImGui::TextWrapped(
            "Selected Node: %s (%s)",
            selectedGraphNode->name.empty()
                ? GetDefaultMaterialGraphNodeName(selectedGraphNode->type)
                : selectedGraphNode->name.c_str(),
            GetMaterialGraphNodeTypeLabel(selectedGraphNode->type));
    }
    else if (selectedGraphLink != nullptr)
    {
        const MaterialShaderNode* fromNode =
            FindMaterialGraphNode(material.shaderGraph, selectedGraphLink->fromNodeId);
        const MaterialShaderNode* toNode =
            FindMaterialGraphNode(material.shaderGraph, selectedGraphLink->toNodeId);
        ImGui::TextWrapped(
            "Selected Link: %s.%s -> %s.%s",
            fromNode != nullptr ? fromNode->name.c_str() : "<missing>",
            selectedGraphLink->fromSlot.c_str(),
            toNode != nullptr ? toNode->name.c_str() : "<missing>",
            selectedGraphLink->toSlot.c_str());
    }
    else
    {
        ImGui::TextDisabled("Tip: right-click the graph background or use Add Node to expand the material graph.");
    }

    if (ImGui::BeginChild(
            "MaterialShaderGraphCanvas",
            ImVec2(0.0f, 660.0f * UiScale()),
            true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        changed |= DrawMaterialGraphCanvas(material, materialIndex);
    }
    ImGui::EndChild();
    return changed;
}

// The canvas inside its child window: pan and zoom, the grid, the nodes and their links, and the
// add-node menu. Returns whether the graph changed.
bool ModelProcessorWindow::DrawMaterialGraphCanvas(ModelImportedMaterialInfo& material, size_t materialIndex)
{
    bool changed = false;
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseMiddle);

    const ImVec2 canvasOrigin =
        ImVec2(
            ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x,
            ImGui::GetWindowPos().y + ImGui::GetWindowContentRegionMin().y);
    const ImVec2 canvasMax =
        ImVec2(
            ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x,
            ImGui::GetWindowPos().y + ImGui::GetWindowContentRegionMax().y);
    const float visibleWidth = canvasMax.x - canvasOrigin.x;
    const float visibleHeight = canvasMax.y - canvasOrigin.y;
    const float gridStep = 48.0f * UiScale() * m_materialGraph.zoom;
    const float nodeUiScale = UiScale() * m_materialGraph.zoom;
    const bool canPasteClipboardNode =
        CanPasteMaterialGraphNode(material.shaderGraph, m_materialGraphClipboardNode);
    const bool mouseOverGraphNode = IsMouseOverMaterialGraphNode(
        material.shaderGraph,
        ImGui::GetIO().MousePos,
        canvasOrigin,
        m_materialGraph.viewOrigin,
        nodeUiScale,
        m_materialGraph.zoom);
    const ImGuiHoveredFlags canvasHoverFlags =
        ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_ChildWindows;
    const bool canvasHovered = ImGui::IsWindowHovered(canvasHoverFlags);
    const bool canvasBackgroundHovered = canvasHovered && !mouseOverGraphNode;
    const ImGuiID canvasInputOwner = ImGui::GetCurrentWindow()->ID;
    if (canvasHovered || m_materialGraph.panningActive)
    {
        ImGui::SetKeyOwner(
            ImGuiKey_MouseWheelY,
            canvasInputOwner,
            ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(
            ImGuiKey_MouseWheelX,
            canvasInputOwner,
            ImGuiInputFlags_LockThisFrame);
        ImGui::SetKeyOwner(
            ImGuiKey_MouseMiddle,
            canvasInputOwner,
            ImGuiInputFlags_LockUntilRelease);
        ImGui::SetNextFrameWantCaptureMouse(true);
    }
    UpdateMaterialGraphView(canvasOrigin, canvasBackgroundHovered);

    if (m_materialGraph.openAddNodePopup)
    {
        m_materialGraph.contextSpawnPosition = MaterialGraphNodePosition{
            m_materialGraph.viewOrigin.x + visibleWidth * 0.28f / m_materialGraph.zoom,
            m_materialGraph.viewOrigin.y + visibleHeight * 0.22f / m_materialGraph.zoom};
        ImGui::OpenPopup("MaterialGraphAddNodePopup");
        m_materialGraph.openAddNodePopup = false;
    }

    if (canvasHovered &&
        !mouseOverGraphNode &&
        !ImGui::IsAnyItemHovered() &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Right))
    {
        m_materialGraph.contextSpawnPosition = ComputeMaterialGraphPositionFromScreen(
            ImGui::GetIO().MousePos,
            canvasOrigin,
            m_materialGraph.viewOrigin,
            m_materialGraph.zoom);
        ImGui::OpenPopup("MaterialGraphAddNodePopup");
    }

    ImDrawList* graphDrawList = ImGui::GetWindowDrawList();
    graphDrawList->PushClipRect(canvasOrigin, canvasMax, true);
    DrawMaterialGraphGrid(graphDrawList, canvasOrigin, canvasMax, m_materialGraph.viewOrigin, m_materialGraph.zoom, gridStep);

    uint32_t pendingDeleteNodeId = 0;
    bool connectionCompletedThisFrame = false;
    bool graphNodeCapturedMouse = false;
    std::optional<MaterialGraphNodePosition> pendingPasteNodePosition;
    std::vector<MaterialGraphRenderedPin> renderedPins;
    renderedPins.reserve(material.shaderGraph.nodes.size() * 8u);

    if (m_materialGraph.nodeResizeActive)
    {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_materialGraph.CancelResize();
        }
        else if (MaterialShaderNode* resizingNode =
                     FindMaterialGraphNode(material.shaderGraph, m_materialGraph.resizeNodeId);
                 resizingNode != nullptr)
        {
            const ImVec2 resizeMouseDelta(
                ImGui::GetIO().MousePos.x - m_materialGraph.resizeStartMouse.x,
                ImGui::GetIO().MousePos.y - m_materialGraph.resizeStartMouse.y);
            changed |= ApplyMaterialGraphNodeResize(
                *resizingNode,
                m_materialGraph.resizeEdges,
                m_materialGraph.resizeStartPosition,
                m_materialGraph.resizeStartSize,
                resizeMouseDelta,
                UiScale(),
                m_materialGraph.zoom);
            graphNodeCapturedMouse = true;
            ImGui::SetMouseCursor(GetMaterialGraphResizeCursor(m_materialGraph.resizeEdges));
        }
        else
        {
            m_materialGraph.CancelResize();
        }
    }

    for (MaterialShaderNode& node : material.shaderGraph.nodes)
    {
        MaterialGraphNodeDrawResult drawResult = DrawMaterialGraphNode(
            material,
            node,
            m_modelPath,
            static_cast<uint32_t>(materialIndex),
            canvasOrigin,
            m_materialGraph.viewOrigin,
            nodeUiScale,
            m_materialGraph.zoom,
            m_materialGraph.selectedNodeId == node.id,
            m_materialGraph.linkDragActive,
            m_materialGraph.nodeResizeActive && m_materialGraph.resizeNodeId == node.id,
            canPasteClipboardNode,
            m_materialGraph.linkDragFromNodeId,
            m_materialGraph.linkDragFromSlot,
            &m_statusMessage);
        changed |= drawResult.changed;
        graphNodeCapturedMouse |= drawResult.capturesMouse;
        if (drawResult.selected)
        {
            m_materialGraph.selectedNodeId = node.id;
            if (drawResult.selectedLinkId == 0)
            {
                m_materialGraph.selectedLinkId = 0;
            }
        }
        if (drawResult.selectedLinkId != 0)
        {
            m_materialGraph.selectedLinkId = drawResult.selectedLinkId;
        }
        if (drawResult.requestDelete)
        {
            pendingDeleteNodeId = node.id;
        }
        if (drawResult.requestCopy)
        {
            m_materialGraphClipboardNode = node;
            m_statusMessage =
                "Copied " + std::string(GetMaterialGraphNodeTypeLabel(node.type)) + " node.";
        }
        if (drawResult.requestPaste)
        {
            pendingPasteNodePosition = drawResult.pastePosition;
        }
        if (drawResult.requestStartResize)
        {
            m_materialGraph.nodeResizeActive = true;
            m_materialGraph.resizeNodeId = node.id;
            m_materialGraph.resizeEdges = drawResult.resizeEdges;
            m_materialGraph.resizeStartPosition = node.position;
            m_materialGraph.resizeStartSize = GetMaterialGraphNodeLogicalSize(node);
            m_materialGraph.resizeStartMouse = ImGui::GetIO().MousePos;
            m_materialGraph.selectedNodeId = node.id;
            m_materialGraph.selectedLinkId = 0;
            if (m_materialGraph.linkDragActive)
            {
                m_materialGraph.CancelLinkDrag();
            }
            graphNodeCapturedMouse = true;
        }
        if (drawResult.requestStartLinkDrag)
        {
            m_materialGraph.linkDragActive = true;
            m_materialGraph.linkDragFromNodeId = drawResult.startLinkNodeId;
            m_materialGraph.linkDragFromSlot = drawResult.startLinkSlot;
            m_materialGraph.CancelResize();
            m_materialGraph.selectedLinkId = 0;
        }
        if (drawResult.connectedLinkId != 0)
        {
            connectionCompletedThisFrame = true;
            m_materialGraph.CancelLinkDrag();
            m_materialGraph.selectedLinkId = drawResult.connectedLinkId;
        }
        renderedPins.insert(
            renderedPins.end(),
            drawResult.pins.begin(),
            drawResult.pins.end());
    }

    if (pendingDeleteNodeId != 0)
    {
        RemoveMaterialGraphNode(material.shaderGraph, pendingDeleteNodeId);
        if (m_materialGraph.selectedNodeId == pendingDeleteNodeId)
        {
            m_materialGraph.selectedNodeId = 0;
        }
        if (m_materialGraph.linkDragActive &&
            m_materialGraph.linkDragFromNodeId == pendingDeleteNodeId)
        {
            m_materialGraph.CancelLinkDrag();
        }
        if (m_materialGraph.nodeResizeActive &&
            m_materialGraph.resizeNodeId == pendingDeleteNodeId)
        {
            m_materialGraph.CancelResize();
        }
        if (m_materialGraph.selectedLinkId != 0 &&
            FindMaterialGraphLink(material.shaderGraph, m_materialGraph.selectedLinkId) == nullptr)
        {
            m_materialGraph.selectedLinkId = 0;
        }
        changed = true;
    }

    for (const MaterialShaderLink& link : material.shaderGraph.links)
    {
        const MaterialGraphRenderedPin* fromPin =
            FindRenderedMaterialGraphPin(renderedPins, link.fromNodeId, link.fromSlot, false);
        const MaterialGraphRenderedPin* toPin =
            FindRenderedMaterialGraphPin(renderedPins, link.toNodeId, link.toSlot, true);
        if (fromPin == nullptr || toPin == nullptr)
        {
            continue;
        }

        const ImU32 linkColor =
            m_materialGraph.selectedLinkId == link.id
                ? kSelectionOutlineColor
                : GetMaterialGraphPinColor(fromPin->kind);
        DrawNodeConnection(
            graphDrawList,
            fromPin->center,
            toPin->center,
            linkColor,
            m_materialGraph.selectedLinkId == link.id
                ? 3.6f * nodeUiScale
                : 2.6f * nodeUiScale);
    }

    if (m_materialGraph.linkDragActive)
    {
        const MaterialGraphRenderedPin* dragFromPin = FindRenderedMaterialGraphPin(
            renderedPins,
            m_materialGraph.linkDragFromNodeId,
            m_materialGraph.linkDragFromSlot,
            false);
        if (dragFromPin != nullptr)
        {
            DrawNodeConnection(
                graphDrawList,
                dragFromPin->center,
                ImGui::GetIO().MousePos,
                kSelectionOutlineColor,
                2.4f * nodeUiScale);
        }
        else
        {
            m_materialGraph.CancelLinkDrag();
        }
    }
    graphDrawList->PopClipRect();

    if (canvasBackgroundHovered &&
        !graphNodeCapturedMouse &&
        !ImGui::IsAnyItemHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        m_materialGraph.selectedNodeId = 0;
        m_materialGraph.selectedLinkId = 0;
    }

    if (m_materialGraph.linkDragActive &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        !connectionCompletedThisFrame)
    {
        m_materialGraph.CancelLinkDrag();
    }

    changed |= DrawMaterialGraphAddNodePopup(material, canPasteClipboardNode, pendingPasteNodePosition);
    // A paste asked for by the shortcut or by the menu above, this frame.
    if (pendingPasteNodePosition.has_value() && canPasteClipboardNode && m_materialGraphClipboardNode.has_value())
    {
        if (MaterialShaderNode* pastedNode = PasteMaterialGraphNode(
                material.shaderGraph,
                *m_materialGraphClipboardNode,
                *pendingPasteNodePosition);
            pastedNode != nullptr)
        {
            m_materialGraph.selectedNodeId = pastedNode->id;
            m_materialGraph.selectedLinkId = 0;
            changed = true;
            m_statusMessage =
                "Pasted " + std::string(GetMaterialGraphNodeTypeLabel(pastedNode->type)) + " node copy.";
        }
        pendingPasteNodePosition.reset();
    }
    return changed;
}

// Middle-drag pans the canvas; the wheel zooms about the cursor.
void ModelProcessorWindow::UpdateMaterialGraphView(const ImVec2& canvasOrigin, bool canvasBackgroundHovered)
{
    if (canvasBackgroundHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
    {
        m_materialGraph.panningActive = true;
    }
    if (m_materialGraph.panningActive)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle))
        {
            m_materialGraph.viewOrigin.x -= ImGui::GetIO().MouseDelta.x / m_materialGraph.zoom;
            m_materialGraph.viewOrigin.y -= ImGui::GetIO().MouseDelta.y / m_materialGraph.zoom;
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        else
        {
            m_materialGraph.panningActive = false;
        }
    }
    if (canvasBackgroundHovered && std::abs(ImGui::GetIO().MouseWheel) > 0.0f)
    {
        const ImVec2 mousePosition = ImGui::GetIO().MousePos;
        const MaterialGraphNodePosition graphPositionBeforeZoom =
            ComputeMaterialGraphPositionFromScreen(
                mousePosition,
                canvasOrigin,
                m_materialGraph.viewOrigin,
                m_materialGraph.zoom);
        const float zoomFactor = std::pow(kMaterialGraphZoomStep, ImGui::GetIO().MouseWheel);
        m_materialGraph.zoom = std::clamp(
            m_materialGraph.zoom * zoomFactor,
            kMaterialGraphMinZoom,
            kMaterialGraphMaxZoom);
        m_materialGraph.viewOrigin.x =
            graphPositionBeforeZoom.x -
            (mousePosition.x - canvasOrigin.x) / m_materialGraph.zoom;
        m_materialGraph.viewOrigin.y =
            graphPositionBeforeZoom.y -
            (mousePosition.y - canvasOrigin.y) / m_materialGraph.zoom;
    }
}

// The background context menu: add a node where it was opened, or paste the copied one there.
// Returns whether the graph changed; a paste is left in `pendingPasteNodePosition`.
bool ModelProcessorWindow::DrawMaterialGraphAddNodePopup(
    ModelImportedMaterialInfo& material,
    bool canPasteClipboardNode,
    std::optional<MaterialGraphNodePosition>& pendingPasteNodePosition)
{
    bool changed = false;
    if (ImGui::BeginPopup("MaterialGraphAddNodePopup"))
    {
        const auto addGraphNode = [&](MaterialShaderNodeType type)
        {
            if (MaterialShaderNode* newNode = AddMaterialGraphNode(
                    material.shaderGraph,
                    type,
                    m_materialGraph.contextSpawnPosition);
                newNode != nullptr)
            {
                m_materialGraph.selectedNodeId = newNode->id;
                m_materialGraph.selectedLinkId = 0;
                changed = true;
                m_statusMessage =
                    "Added " + std::string(GetMaterialGraphNodeTypeLabel(type)) + " node.";
            }
            ImGui::CloseCurrentPopup();
        };

        if (ImGui::MenuItem("Texture Node"))
        {
            addGraphNode(MaterialShaderNodeType::Texture);
        }
        if (ImGui::MenuItem("Scalar Node"))
        {
            addGraphNode(MaterialShaderNodeType::Scalar);
        }
        if (ImGui::MenuItem("Color Node"))
        {
            addGraphNode(MaterialShaderNodeType::Color);
        }
        if (ImGui::MenuItem("Surface Node"))
        {
            addGraphNode(MaterialShaderNodeType::Surface);
        }
        if (ImGui::MenuItem("Blend Node"))
        {
            addGraphNode(MaterialShaderNodeType::Blend);
        }
        ImGui::BeginDisabled(MaterialGraphHasOutputNode(material.shaderGraph));
        if (ImGui::MenuItem("Output Node"))
        {
            addGraphNode(MaterialShaderNodeType::Output);
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::BeginMenu("Edit"))
        {
            if (ImGui::MenuItem("Paste Node", nullptr, false, canPasteClipboardNode))
            {
                pendingPasteNodePosition = m_materialGraph.contextSpawnPosition;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    return changed;
}
}
