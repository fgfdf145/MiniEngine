#include "editor_ui.h"
#include "editor_ui_internal.h"

#include <material_graph_runtime.h>
#include <model_loader.h>
#include <texture_loader.h>

#include <editor_world.h>
#include <file_dialog/file_dialog.h>
#include <log/log.h>
#include <ui/ui_scale.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <yaml-cpp/yaml.h>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/common.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace
{
bool MaterialHasAnyTexture(const ModelImportedMaterialInfo& material)
{
    return
        !material.baseColorTexturePath.empty() ||
        !material.normalTexturePath.empty() ||
        !material.metallicTexturePath.empty() ||
        !material.roughnessTexturePath.empty() ||
        !material.occlusionTexturePath.empty() ||
        !material.emissiveTexturePath.empty();
}

uint32_t CountUvReadySubmeshes(const EditorModelMetadataComponent& metadata)
{
    return static_cast<uint32_t>(std::count_if(
        metadata.importedSubmeshes.begin(),
        metadata.importedSubmeshes.end(),
        [](const ModelImportedSubmeshInfo& submesh)
        {
            return submesh.hasTexCoords;
        }
    ));
}

uint32_t CountTexturedMaterials(const EditorModelMetadataComponent& metadata)
{
    return static_cast<uint32_t>(std::count_if(
        metadata.importedMaterials.begin(),
        metadata.importedMaterials.end(),
        [](const ModelImportedMaterialInfo& material)
        {
            return MaterialHasAnyTexture(material);
        }
    ));
}

void DrawImportedModelInspector(const EditorModelMetadataComponent& metadata)
{
    if (metadata.importedSubmeshes.empty() && metadata.importedMaterials.empty())
    {
        return;
    }

    ImGui::Separator();
    ImGui::Text("Importer: %s", ModelLoader::GetImporterName());
    ImGui::Text(
        "UV Submeshes: %u / %u",
        CountUvReadySubmeshes(metadata),
        static_cast<unsigned int>(metadata.importedSubmeshes.size())
    );
    ImGui::Text(
        "Textured Materials: %u / %u",
        CountTexturedMaterials(metadata),
        static_cast<unsigned int>(metadata.importedMaterials.size())
    );
    ImGui::TextWrapped("Texture bindings are applied only when the imported submesh carries valid UVs.");

    if (ImGui::TreeNode("Imported Submeshes"))
    {
        for (size_t submeshIndex = 0; submeshIndex < metadata.importedSubmeshes.size(); ++submeshIndex)
        {
            const ModelImportedSubmeshInfo& submesh = metadata.importedSubmeshes[submeshIndex];
            const std::string treeLabel =
                submesh.name.empty()
                ? ("Submesh " + std::to_string(submeshIndex))
                : (submesh.name + "##submesh_" + std::to_string(submeshIndex));
            if (!ImGui::TreeNode(treeLabel.c_str()))
            {
                continue;
            }

            ImGui::Text("Vertices: %u", submesh.vertexCount);
            ImGui::Text("Indices: %u", submesh.indexCount);
            ImGui::Text("Triangles: %u", submesh.indexCount / 3u);
            ImGui::Text("Material Slot: %u", submesh.materialIndex);
            ImGui::Text("Has UV: %s", submesh.hasTexCoords ? "Yes" : "No");
            ImGui::Text("Has Normal: %s", submesh.hasNormals ? "Yes" : "No");
            ImGui::Text("Has Tangent: %s", submesh.hasTangents ? "Yes" : "No");
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("Imported Materials"))
    {
        for (size_t materialIndex = 0; materialIndex < metadata.importedMaterials.size(); ++materialIndex)
        {
            const ModelImportedMaterialInfo& material = metadata.importedMaterials[materialIndex];
            const MaterialTextureBlendGraph& blendGraph = material.blendGraph;
            const bool hasProgrammableGraph = HasSecondaryMaterialLayer(blendGraph);
            const std::string materialName = material.name.empty()
                ? ("Material " + std::to_string(materialIndex))
                : material.name;
            const std::string treeLabel =
                (hasProgrammableGraph ? "[PBR Graph] " : "") + materialName + "##material_" + std::to_string(materialIndex);
            if (!ImGui::TreeNode(treeLabel.c_str()))
            {
                continue;
            }

            DrawPrimaryMaterialTextureRows(material);
            if (hasProgrammableGraph)
            {
                ImGui::Separator();
                ImGui::Text("Programmable Blend: %s", blendGraph.enabled ? "Enabled" : "Prepared");
                ImGui::Text("Blend Factor: %.2f", blendGraph.blendFactor);
                DrawSecondaryMaterialTextureRows(blendGraph);
            }
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }
}

bool DrawOperationButton(const char* label, ImGuizmo::OPERATION value, ImGuizmo::OPERATION& current)
{
    const bool selected = current == value;
    if (ImGui::RadioButton(label, selected))
    {
        current = value;
        return true;
    }

    return false;
}

bool DrawTransformComponent(TransformComponent& transform)
{
    bool changed = ImGui::DragFloat3(
        "Translation (m)",
        glm::value_ptr(transform.translation),
        0.05f,
        -WorldUnits::kUiTransformTranslationRangeMeters,
        WorldUnits::kUiTransformTranslationRangeMeters,
        "%.3f"
    );
    changed |= ImGui::DragFloat3("Rotation", glm::value_ptr(transform.rotationDegrees), 0.5f);
    changed |= ImGui::DragFloat3(
        "Scale (1 = source meters)",
        glm::value_ptr(transform.scale),
        0.02f,
        WorldUnits::kMinimumScale,
        WorldUnits::kUiTransformScaleMax,
        "%.3f"
    );
    const glm::vec3 clampedScale = glm::max(transform.scale, WorldUnits::kMinimumScale3);
    changed |= glm::any(glm::notEqual(clampedScale, transform.scale));
    transform.scale = clampedScale;
    return changed;
}

void DrawGizmoControls(GizmoSettings& gizmo)
{
    DrawOperationButton("Combined", kCombinedGizmoOperation, gizmo.operation);
    ImGui::SameLine();
    DrawOperationButton("Scale", ImGuizmo::SCALE, gizmo.operation);

    const bool worldMode = gizmo.mode == ImGuizmo::WORLD;
    if (ImGui::RadioButton("World", worldMode))
    {
        gizmo.mode = ImGuizmo::WORLD;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Local", !worldMode))
    {
        gizmo.mode = ImGuizmo::LOCAL;
    }

    ImGui::Checkbox("Use Snap", &gizmo.useSnap);
    ImGui::DragFloat3(
        "Move Snap (m)",
        glm::value_ptr(gizmo.translationSnap),
        0.05f,
        WorldUnits::kUiCameraNearMinMeters,
        WorldUnits::kUiTranslationSnapMaxMeters,
        "%.2f"
    );
    ImGui::DragFloat("Rotate Snap", &gizmo.rotationSnap, 0.5f, 1.0f, 90.0f, "%.1f deg");
    ImGui::DragFloat3("Scale Snap", glm::value_ptr(gizmo.scaleSnap), 0.01f, 0.01f, WorldUnits::kUiScaleSnapMax, "%.2f");
}

void DrawLightComponentEditor(LightComponent& light, float uiScale)
{
    // Type selector
    const char* currentTypeLabel = GetLightTypeLabel(light.type);
    if (ImGui::BeginCombo("Type", currentTypeLabel))
    {
        constexpr LightType kTypes[] = {
            LightType::Directional, LightType::Point, LightType::Spot,
            LightType::Area, LightType::Ambient
        };
        for (LightType t : kTypes)
        {
            const bool selected = t == light.type;
            if (ImGui::Selectable(GetLightTypeLabel(t), selected))
            {
                light.type = t;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // Color
    ImGui::ColorEdit3("Color", &light.color.x, ImGuiColorEditFlags_Float);

    // Intensity with unit label
    const char* intensityUnit = "lm";  // lumens for most lights
    if (light.type == LightType::Directional) intensityUnit = "lx";
    if (light.type == LightType::Ambient)     intensityUnit = "x";

    const std::string intensityLabel = std::string("Intensity (") + intensityUnit + ")";
    ImGui::DragFloat(intensityLabel.c_str(), &light.intensity, 10.0f, 0.0f, 1000000.0f, "%.1f");
    light.intensity = std::max(light.intensity, 0.0f);

    // Range
    if (light.type != LightType::Directional && light.type != LightType::Ambient)
    {
        ImGui::DragFloat("Range (m)", &light.range, 0.1f, 0.1f, 1000.0f, "%.2f");
        light.range = std::max(light.range, 0.01f);
    }

    // Spot angles
    if (light.type == LightType::Spot)
    {
        ImGui::SliderFloat("Inner Angle", &light.spotInnerAngleDegrees, 1.0f, 89.0f, "%.1f deg");
        ImGui::SliderFloat("Outer Angle", &light.spotOuterAngleDegrees, 1.0f, 89.0f, "%.1f deg");
        light.spotInnerAngleDegrees = std::clamp(light.spotInnerAngleDegrees, 1.0f, 89.0f);
        light.spotOuterAngleDegrees = std::clamp(light.spotOuterAngleDegrees,
                                                  light.spotInnerAngleDegrees, 89.0f);
    }

    // Area size
    if (light.type == LightType::Area)
    {
        ImGui::DragFloat2("Area Size (m)", &light.areaSize.x, 0.05f, 0.01f, 100.0f, "%.2f");
        light.areaSize = glm::max(light.areaSize, glm::vec2(0.01f));
    }

    // Tip
    ImGui::Spacing();
    ImGui::TextDisabled("Tip: use the viewport gizmo to move/rotate this light.");
}
}

void EditorUiController::DrawScenePanel(IEditorWorld& scene, const std::string& lastSceneIoError, EditorUiFrameResult& result)
{
    if (ImGui::Begin("Scene", &m_showSceneWindow))
    {
        const size_t modelCount = scene.Registry().view<const ModelComponent>().size();
        const size_t lightCount = scene.Registry().view<const LightComponent>().size();
        ImGui::Text("Models: %u  Lights: %u",
            static_cast<unsigned int>(modelCount),
            static_cast<unsigned int>(lightCount));

        if (ImGui::Button("Add Entity"))
        {
            result.actions.createSceneEntity = true;
        }
        ImGui::SameLine();

        // "Add Light" dropdown
        if (ImGui::Button("Add Light"))
        {
            ImGui::OpenPopup("AddLightPopup");
        }
        if (ImGui::BeginPopup("AddLightPopup"))
        {
            const auto addLight = [&](LightType type, const char* name)
            {
                if (ImGui::MenuItem(GetLightTypeLabel(type)))
                {
                    result.actions.createLightEntity = EditorUiActions::LightCreate{
                        std::string(name) + " Light",
                        type
                    };
                }
            };
            addLight(LightType::Point,       "Point");
            addLight(LightType::Directional, "Directional");
            addLight(LightType::Spot,        "Spot");
            addLight(LightType::Area,        "Area");
            addLight(LightType::Ambient,     "Ambient");
            ImGui::EndPopup();
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(!scene.HasSelection());
        if (ImGui::Button("Delete"))
        {
            result.actions.deleteSelectedSceneEntity = true;
        }
        ImGui::EndDisabled();

        ImGui::Separator();

        // --- Model entity list ---
        if (modelCount != 0u)
        {
            ImGui::TextDisabled("Models");
        }
        for (entt::entity entity : scene.GetSceneOrder())
        {
            if (!scene.HasModelComponent(entity))
            {
                continue;
            }
            const TagComponent& tag = scene.GetTag(entity);
            const std::string label = tag.name + "##model_" +
                std::to_string(static_cast<uint32_t>(entt::to_integral(entity)));
            if (ImGui::Selectable(label.c_str(), scene.IsSelected(entity)))
            {
                scene.SetSelectedEntity(entity);
            }
        }

        // --- Light entity list ---
        if (lightCount != 0u)
        {
            if (modelCount != 0u) ImGui::Spacing();
            ImGui::TextDisabled("Lights");
        }
        for (entt::entity entity : scene.GetSceneOrder())
        {
            if (!scene.HasLightComponent(entity))
            {
                continue;
            }
            const TagComponent& tag = scene.GetTag(entity);
            const LightComponent& light = scene.GetLightComponent(entity);
            const ImVec4 typeColor = ImGui::ColorConvertU32ToFloat4(GetLightTypeColor(light.type));
            ImGui::PushStyleColor(ImGuiCol_Text, typeColor);
            const std::string label = std::string("[") + GetLightTypeLabel(light.type)[0] + "] " +
                tag.name + "##light_" +
                std::to_string(static_cast<uint32_t>(entt::to_integral(entity)));
            ImGui::PopStyleColor();
            if (ImGui::Selectable(label.c_str(), scene.IsSelected(entity)))
            {
                scene.SetSelectedEntity(entity);
            }
        }

        // --- Selected entity inspector ---
        if (scene.HasSelection())
        {
            const entt::entity selectedEntity = scene.GetSelectedEntity();
            const bool isLight = scene.HasLightComponent(selectedEntity);

            TagComponent& tag = scene.EditTag(selectedEntity);
            TransformComponent& transform = scene.EditTransform(selectedEntity);

            ImGui::Separator();

            char tagBuffer[128]{};
            std::snprintf(tagBuffer, sizeof(tagBuffer), "%s", tag.name.c_str());
            if (ImGui::InputText("Name", tagBuffer, sizeof(tagBuffer)))
            {
                tag.name = tagBuffer;
            }
            ImGui::TextDisabled("Entity UUID: %s", scene.GetEntityUuid(selectedEntity).c_str());

            if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (DrawTransformComponent(transform))
                {
                    scene.MarkTransformDirty(selectedEntity);
                }
                if (ImGui::Button("Reset Transform"))
                {
                    scene.ResetSelectedTransform();
                }
            }

            if (isLight)
            {
                LightComponent& light = scene.EditLightComponent(selectedEntity);
                if (ImGui::CollapsingHeader("LightComponent", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    DrawLightComponentEditor(light, m_effectiveUiScale);
                }

                // Directional lights use the combined move/rotate gizmo.
                GizmoSettings& gizmo = scene.GetGizmoSettings();
                if (ImGui::CollapsingHeader("GizmoComponent", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    if (light.type == LightType::Point || light.type == LightType::Ambient)
                    {
                        ImGui::TextDisabled("Translate only (no orientation for this light type)");
                    }
                    else
                    {
                        DrawGizmoControls(gizmo);
                    }
                }
            }
            else
            {
                const ModelComponent& model = scene.GetSelectedModel();
                const ModelBoundsComponent& bounds = scene.GetSelectedModelBounds();
                const EditorModelMetadataComponent& metadata = scene.GetSelectedModelMetadata();
                GizmoSettings& gizmo  = scene.GetGizmoSettings();

                if (ImGui::CollapsingHeader("ModelComponent", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    ImGui::TextWrapped("Display Name: %s", model.displayName.c_str());
                    ImGui::TextWrapped("Source Path: %s", model.sourcePath.empty() ? "<builtin cube>" : model.sourcePath.c_str());
                    ImGui::Text("Imported Unit Scale: 1.0 = 1 meter");
                    ImGui::Text("Submeshes: %u", metadata.submeshCount);

                    if (bounds.hasBounds)
                    {
                        ImGui::Text("Bounds Min (m): %.2f %.2f %.2f", bounds.minBounds.x, bounds.minBounds.y, bounds.minBounds.z);
                        ImGui::Text("Bounds Max (m): %.2f %.2f %.2f", bounds.maxBounds.x, bounds.maxBounds.y, bounds.maxBounds.z);
                    }

                    if (!model.sourcePath.empty())
                    {
                        ImGui::TextWrapped("Asset management is disabled while it is being rebuilt.");
                    }

                    DrawImportedModelInspector(metadata);
                }

                if (ImGui::CollapsingHeader("GizmoComponent", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    DrawGizmoControls(gizmo);
                }
            }

        }
        else
        {
            ImGui::TextUnformatted("No entity selected.");
        }

        // Scene I/O — always visible
        ImGui::Separator();
        ImGui::TextWrapped("Scene: %s", scene.GetSceneFilePath().empty() ? "<unsaved>" : scene.GetSceneFilePath().c_str());
        if (!lastSceneIoError.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Error: %s", lastSceneIoError.c_str());
        }
        if (ImGui::Button("Save Scene"))
        {
            // Save to current path if already set, otherwise open dialog
            if (!scene.GetSceneFilePath().empty())
            {
                result.actions.selectedSceneSavePath = scene.GetSceneFilePath();
            }
            else if (const std::optional<std::string> savePath = SaveSceneFileDialog(); savePath.has_value())
            {
                result.actions.selectedSceneSavePath = *savePath;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Save As..."))
        {
            if (const std::optional<std::string> savePath = SaveSceneFileDialog(); savePath.has_value())
            {
                result.actions.selectedSceneSavePath = *savePath;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Load Scene"))
        {
            if (const std::optional<std::string> loadPath = OpenSceneFileDialog(); loadPath.has_value())
            {
                result.actions.selectedSceneLoadPath = *loadPath;
            }
        }
    }
    ImGui::End();
}
