#include "scene_panel.h"

#include <engine/editor/editor_ui.h>
#include <engine/editor/ui/editor_ui_internal.h>
#include <engine/editor/ui/framework/editor_window_manager.h>
#include <engine/editor/ui/windows/model_processor_window.h>
#include <engine/editor/services/model_animation_service.h>

#include <engine/asset/asset_registry.h>
#include <engine/asset/model_cache.h>
#include <engine/asset/model_loader.h>

#include <engine/editor/ui_colors.h>
#include <engine/logic/editor_world.h>
#include <engine/platform/file_dialog/file_dialog.h>
#include <engine/scene/sun_position.h>
#include <engine/scene/wind.h>
#include <IconsPhosphor.h>
#include <fmt/format.h>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <ImGuizmo.h>
#include <glm/common.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <utility>
#include <vector>

namespace me
{

namespace
{
bool MaterialHasAnyTexture(const ModelImportedMaterialInfo& material)
{
    return !material.baseColorTexturePath.empty() ||
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
        }));
}

uint32_t CountTexturedMaterials(const EditorModelMetadataComponent& metadata)
{
    return static_cast<uint32_t>(std::count_if(
        metadata.importedMaterials.begin(),
        metadata.importedMaterials.end(),
        [](const ModelImportedMaterialInfo& material)
        {
            return MaterialHasAnyTexture(material);
        }));
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
        static_cast<unsigned int>(metadata.importedSubmeshes.size()));
    ImGui::Text(
        "Textured Materials: %u / %u",
        CountTexturedMaterials(metadata),
        static_cast<unsigned int>(metadata.importedMaterials.size()));
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

// glTF animations (ModelAnimationPlayback): the clip the selected model plays, whether it plays, and
// how fast. Shown for a model with skins and clips.
void DrawModelAnimationControls(const ModelComponent& model, EditorUiFrameResult& result)
{
    if (model.sourcePath.empty())
    {
        return;
    }
    const std::shared_ptr<const LoadedModelData> data = ModelCache::Get(model.sourcePath);
    if (!data || !data->skeleton || data->skeleton->clips.empty() || data->skeleton->bindings.empty())
    {
        return;
    }
    const ModelSkeleton& skeleton = *data->skeleton;
    EditorUiActions::ModelAnimationChoice choice{
        model.animationClip, model.animationEnabled, model.animationPlaying, model.animationSpeed, model.springBones};
    bool changed = false;

    ImGui::SeparatorText("Animation");
    changed |= ImGui::Checkbox("Animate", &choice.enabled);
    ImGui::SetItemTooltip("Off, the skinned meshes keep their bind pose.");
    ImGui::BeginDisabled(!choice.enabled);
    const int32_t automatic = ModelAnimationPlayback::ResolveClip(skeleton, std::string{});
    const std::string automaticLabel =
        "Default (" + (automatic >= 0 ? skeleton.clips[static_cast<size_t>(automatic)].name : std::string("none")) + ")";
    const std::string preview = choice.clip.empty() ? automaticLabel : choice.clip;
    if (ImGui::BeginCombo("Clip", preview.c_str(), ImGuiComboFlags_HeightLarge))
    {
        if (ImGui::Selectable(automaticLabel.c_str(), choice.clip.empty()) && !choice.clip.empty())
        {
            choice.clip.clear();
            changed = true;
        }
        for (size_t index = 0; index < skeleton.clips.size(); ++index)
        {
            const ModelAnimationClip& clip = skeleton.clips[index];
            const std::string label = fmt::format("{}  ({:.1f} s)##clip_{}", clip.name, clip.duration, index);
            if (ImGui::Selectable(label.c_str(), clip.name == choice.clip) && clip.name != choice.clip)
            {
                choice.clip = clip.name;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    changed |= ImGui::Checkbox("Playing", &choice.playing);
    ImGui::SameLine();
    changed |= ImGui::DragFloat("Speed", &choice.speed, 0.01f, -4.0f, 4.0f, "%.2fx");
    ImGui::EndDisabled();
    changed |= ImGui::Checkbox("Hair and Skirt Physics", &choice.springBones);
    ImGui::SetItemTooltip("Hair, skirts and other dangling parts swing as the body moves; off, they keep the pose as it is animated.");
    if (changed)
    {
        result.actions.selectedModelAnimation = choice;
    }
}

// A seated driver (VehicleDriverService): the car the selected model drives, its seat offset and why it
// is not sitting in it. Shown for a model with skins; any model with a steering wheel is a car.
void DrawModelDriverControls(
    const IEditorWorld& scene,
    entt::entity entity,
    const ModelComponent& model,
    EditorSharedState& state,
    EditorUiFrameResult& result)
{
    const std::unordered_map<entt::entity, std::string>& problems = state.driverProblems;
    if (model.sourcePath.empty())
    {
        return;
    }
    const std::shared_ptr<const LoadedModelData> data = ModelCache::Get(model.sourcePath);
    if (!data || !data->skeleton || data->skeleton->bindings.empty())
    {
        return;
    }
    EditorUiActions::ModelDriverChoice choice{model.driverVehicleUuid, model.driverSeatOffset, model.driverGrip};
    bool changed = false;

    ImGui::SeparatorText("Driver");
    std::string preview = "None";
    if (!choice.vehicleUuid.empty())
    {
        preview = "(not in the scene)";
    }
    std::vector<std::pair<std::string, std::string>> cars;
    for (const entt::entity other : scene.GetSceneOrder())
    {
        if (other == entity || !scene.HasModelComponent(other) || !scene.Registry().all_of<SceneEntityIdComponent>(other))
        {
            continue;
        }
        const std::shared_ptr<const LoadedModelData> otherData = ModelCache::Get(scene.GetModel(other).sourcePath);
        if (!otherData || !otherData->steeringWheel.has_value())
        {
            continue;
        }
        cars.emplace_back(scene.GetEntityUuid(other), scene.GetTag(other).name);
        if (cars.back().first == choice.vehicleUuid)
        {
            preview = cars.back().second;
        }
    }
    if (ImGui::BeginCombo("Drives", preview.c_str()))
    {
        if (ImGui::Selectable("None", choice.vehicleUuid.empty()) && !choice.vehicleUuid.empty())
        {
            choice.vehicleUuid.clear();
            changed = true;
        }
        for (size_t index = 0; index < cars.size(); ++index)
        {
            const std::string label = cars[index].second + "##car_" + std::to_string(index);
            if (ImGui::Selectable(label.c_str(), cars[index].first == choice.vehicleUuid) && cars[index].first != choice.vehicleUuid)
            {
                choice.vehicleUuid = cars[index].first;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Sits the model in the car's driver's seat, its hands on the steering wheel. It follows the car, and a drive "
                          "turns the wheel in its hands; from the cockpit the camera is at its eyes.");
    ImGui::BeginDisabled(choice.vehicleUuid.empty());
    changed |= ImGui::DragFloat3("Seat Offset (m)", &choice.seatOffset.x, 0.005f, -0.5f, 0.5f, "%.3f");
    ImGui::SetItemTooltip("Moves the hips from where the seat puts them: to the car's right, up, forward.");
    if (ImGui::TreeNode("Grip Calibration"))
    {
        // The left hand's; the right hand mirrors it.
        DriverGripCalibration& grip = choice.grip;
        // The transform gizmo on a wrist, instead of on the entity: move and turn it in the viewport.
        const bool canGizmo = state.driverGrips.count(entity) != 0;
        ImGui::BeginDisabled(!canGizmo);
        for (int side = 0; side < 2; ++side)
        {
            if (side == 1)
            {
                ImGui::SameLine();
            }
            const bool active = state.driverWristGizmo == side;
            const char* label = side == 0 ? (active ? "Done##leftwrist" : "Move Left Wrist") : (active ? "Done##rightwrist" : "Move Right Wrist");
            if (ImGui::Button(label))
            {
                state.driverWristGizmo = active ? -1 : side;
            }
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Puts the transform gizmo on that wrist: move and turn the hand on the wheel in the viewport (the other hand mirrors it).");
        changed |= ImGui::SliderFloat("Hold At (deg)", &grip.holdAtDegrees, 30.0f, 150.0f, "%.0f");
        ImGui::SetItemTooltip("Where the hands hold the rim at rest, from the top: 90 is a quarter to three, 60 ten to two.");
        glm::vec3 offsetCm = grip.wristOffset * 100.0f;
        if (ImGui::DragFloat3("Wrist Offset (cm)", &offsetCm.x, 0.1f, -20.0f, 20.0f, "%.1f"))
        {
            grip.wristOffset = offsetCm / 100.0f;
            changed = true;
        }
        ImGui::SetItemTooltip("Moves the wrist from where the grip is fitted: out from the wheel's centre, along the rim, along the column.");
        changed |= ImGui::DragFloat3("Wrist Turn (deg)", &grip.wristTurnDegrees.x, 0.5f, -180.0f, 180.0f, "%.0f");
        ImGui::SetItemTooltip("Turns the hand about the wrist, about the same three directions.");
        changed |= ImGui::SliderFloat("Finger Curl (deg)", &grip.fingerCurlDegrees, -45.0f, 45.0f, "%.0f");
        ImGui::SetItemTooltip("Added to every finger joint's bend; negative opens the hand.");
        changed |= ImGui::Checkbox("Thumb on Rim", &grip.thumbOnRim);
        if (ImGui::Button("Reset Grip"))
        {
            grip = DriverGripCalibration{};
            changed = true;
        }
        ImGui::TreePop();
    }
    ImGui::EndDisabled();
    if (!choice.vehicleUuid.empty())
    {
        if (const auto found = problems.find(entity); found != problems.end())
        {
            ImGui::TextWrapped("Not seated: %s.", found->second.c_str());
        }
        else
        {
            ImGui::TextDisabled("Its transform follows the car.");
        }
    }
    if (changed)
    {
        result.actions.selectedModelDriver = choice;
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
        "%.3f",
        ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::DragFloat3("Rotation", glm::value_ptr(transform.rotationDegrees), 0.5f);
    changed |= ImGui::DragFloat3(
        "Scale (1 = source meters)",
        glm::value_ptr(transform.scale),
        0.02f,
        WorldUnits::kMinimumScale,
        WorldUnits::kUiTransformScaleMax,
        "%.3f",
        ImGuiSliderFlags_AlwaysClamp);
    const glm::vec3 clampedScale = glm::max(transform.scale, WorldUnits::kMinimumScale3);
    changed |= glm::any(glm::notEqual(clampedScale, transform.scale));
    transform.scale = clampedScale;
    return changed;
}

bool SceneHasDirectionalLight(const IEditorWorld& scene)
{
    bool found = false;
    scene.ForEachLight(
        [&](entt::entity, const TagComponent&, const TransformComponent&, const LightComponent& light)
        {
            found = found || light.type == LightType::Directional;
        });
    return found;
}

// The scene's sky. Edits a copy and writes it back only when something changed.
void DrawEnvironmentEditor(IEditorWorld& scene)
{
    SceneEnvironment environment = scene.GetEnvironment();

    static constexpr std::array<const char*, 3> kModes = {"None", "Atmosphere", "HDRI"};
    int mode = static_cast<int>(environment.mode);
    if (ImGui::Combo("Sky", &mode, kModes.data(), static_cast<int>(kModes.size())))
    {
        environment.mode = static_cast<EnvironmentMode>(mode);
    }
    DragFloatInRange("Exposure compensation (EV)", &environment.exposureCompensationEv, -5.0f, 5.0f, "%+.1f");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Added to the camera's auto exposure compensation in this scene: the meter makes\na white studio grey, +3 keeps it white. Manual exposure ignores it.");
    }

    if (ImGui::CollapsingHeader("Time of day"))
    {
        TimeOfDaySettings& time = environment.timeOfDay;
        ImGui::PushID("TimeOfDay");
        ImGui::Checkbox("Enabled", &time.enabled);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Turns the sun (the brightest Directional light) along a\nnorthern-hemisphere arc by the clock; its rotation follows.");
        }
        ImGui::BeginDisabled(!time.enabled);
        const int minutes = static_cast<int>(time.hours * 60.0f + 0.5f) % (24 * 60);
        char clock[16];
        std::snprintf(clock, sizeof(clock), "%02d:%02d", minutes / 60, minutes % 60);
        ImGui::SliderFloat("Time (h)", &time.hours, 0.0f, 24.0f, clock, ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderInt("Day of year", &time.dayOfYear, 1, 365, "%d", ImGuiSliderFlags_AlwaysClamp);
        DragFloatInRange("Latitude N (deg)", &time.latitudeDegrees, 0.0f, 90.0f, "%.1f");
        DragFloatInRange("North (deg)", &time.northDegrees, -180.0f, 180.0f, "%.1f", 0.5f);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Where north lies: 0 is -Z (south +Z, east +X); positive turns it about +Y.");
        }
        ImGui::DragFloat("Time scale", &time.timeScale, 1.0f, 0.0f, 86400.0f, "%.0fx", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Scene seconds per real second; 0 holds the clock. 3600 is an hour a second.");
        }
        const SolarAngles sun = ComputeSolarAngles(time);
        ImGui::TextDisabled("Sun: elevation %.1f deg, azimuth %.1f deg", sun.elevationDegrees, sun.azimuthDegrees);

        ImGui::Checkbox("Moon", &time.moonEnabled);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Once the sun is %.0f deg below the horizon the moon lights the scene\nin its place: the sky, the shadows and the disk.", -kMoonTakesOverSunElevationDegrees);
        }
        ImGui::BeginDisabled(!time.moonEnabled);
        static constexpr std::array<const char*, 8> kPhaseNames = {
            "new", "waxing crescent", "first quarter", "waxing gibbous", "full", "waning gibbous", "last quarter", "waning crescent"};
        const int phaseIndex = static_cast<int>(std::floor(time.moonPhase * 8.0f + 0.5f)) % 8;
        DragFloatInRange("Moon phase", &time.moonPhase, 0.0f, 1.0f, kPhaseNames[static_cast<size_t>(std::max(phaseIndex, 0))]);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("0 new, 0.25 first quarter, 0.5 full (up all night), 0.75 last quarter.");
        }
        ImGui::DragFloat("Moon brightness", &time.moonBrightness, 0.05f, 0.0f, 100.0f, "%.2fx", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        const SolarAngles moon = ComputeLunarAngles(time);
        ImGui::TextDisabled(
            "Moon: elevation %.1f deg, azimuth %.1f deg, %.0f%% of full%s",
            moon.elevationDegrees,
            moon.azimuthDegrees,
            100.0f * MoonPhaseIlluminanceFraction(time.moonPhase),
            ComputeMoonlight(time).has_value() ? " (lighting)" : "");
        ImGui::EndDisabled();
        ImGui::DragFloat("Night sky (cd/m2)", &time.nightSkyLuminance, 0.0005f, 0.0f, 1.0f, "%.4f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Airglow and starlight, so a moonless night is not black. The real sky is about 0.0002.");
        }
        ImGui::EndDisabled();
        if (time.enabled && !SceneHasDirectionalLight(scene))
        {
            ImGui::TextDisabled("Add a Directional light: it is the sun.");
        }
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Wind"))
    {
        WindSettings& wind = environment.wind;
        ImGui::PushID("Wind");
        ImGui::DragFloat("Speed at 10 m (m/s)", &wind.speed, 0.1f, 0.0f, kWindMaxSpeed, "%.1f", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Rises with height through the boundary layer, %.2fx at %.0f m and above.",
                              WindSpeedAt(WindSettings{1.0f, 0.0f}, kWindBoundaryLayerMeters), kWindBoundaryLayerMeters);
        }
        DragFloatInRange("From (deg)", &wind.fromDegrees, 0.0f, 360.0f, "%.0f", 1.0f);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("The bearing it blows from, clockwise from north (Time of day > North):\n0 a north wind, 90 east, 180 south, 270 west.");
        }
        const glm::vec3 toward = WindDirection(wind, environment.timeOfDay.northDegrees);
        ImGui::TextDisabled("Toward world (%.2f, %.2f), %.1f m/s at the clouds", toward.x, toward.z,
                            WindSpeedAt(wind, environment.clouds.baseAltitude + 0.5f * environment.clouds.thickness));
        ImGui::PopID();
    }

    if (environment.mode == EnvironmentMode::Atmosphere)
    {
        AtmosphereSettings& atmosphere = environment.atmosphere;
        if (!SceneHasDirectionalLight(scene))
        {
            ImGui::TextDisabled("Add a Directional light: it is the sun.");
        }
        ImGui::ColorEdit3("Ground albedo", &atmosphere.groundAlbedo.x);
        ImGui::Checkbox("Ground plane", &atmosphere.groundPlane);
        // A ground plane is a surface, and the sky's own ground beyond it must match it.
        ImGui::BeginDisabled(atmosphere.groundPlane);
        ImGui::Checkbox("Seamless horizon", &atmosphere.seamlessHorizon);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip(
                "No ground in the sky: below the horizon it mirrors the sky above,\nand the sky's ambient light has no bounce off the ground.\nOff while the ground plane is on.");
        }
        ImGui::DragFloat("Rayleigh density", &atmosphere.rayleighDensityScale, 0.01f, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::DragFloat("Mie density", &atmosphere.mieDensityScale, 0.01f, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        DragFloatInRange("Mie anisotropy", &atmosphere.mieAnisotropy, 0.0f, 0.99f, "%.2f");
        ImGui::DragFloat("Ozone density", &atmosphere.ozoneDensityScale, 0.01f, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::DragFloat(
            "Aerial perspective scale",
            &atmosphere.aerialPerspectiveDistanceScale,
            1.0f,
            0.0f,
            10000.0f,
            "%.1f",
            ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        DragFloatInRange("Sun disk (deg)", &atmosphere.sunAngularDiameterDegrees, 0.1f, 5.0f, "%.3f");

        if (ImGui::CollapsingHeader("Height fog"))
        {
            HeightFogSettings& fog = environment.heightFog;
            ImGui::PushID("HeightFog");
            ImGui::Checkbox("Enabled", &fog.enabled);
            constexpr ImGuiSliderFlags kLog = ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp;
            ImGui::DragFloat("Density (/m)", &fog.density, 0.0001f, 0.0f, 0.1f, "%.5f", kLog);
            ImGui::DragFloat("Height falloff (/m)", &fog.heightFalloff, 0.0005f, 1e-4f, 1.0f, "%.4f", kLog);
            ImGui::DragFloat("Fog height (m)", &fog.fogHeight, 0.5f, -10000.0f, 10000.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::DragFloat("Start distance (m)", &fog.startDistance, 1.0f, 0.0f, 100000.0f, "%.0f", kLog);
            DragFloatInRange("Max opacity", &fog.maxOpacity, 0.0f, 1.0f, "%.2f");
            ImGui::ColorEdit3("Albedo", &fog.albedo.x);
            DragFloatInRange("Anisotropy", &fog.anisotropy, 0.0f, 0.95f, "%.2f");
            ImGui::PopID();
        }

        if (ImGui::CollapsingHeader("Clouds"))
        {
            CloudSettings& clouds = environment.clouds;
            ImGui::PushID("Clouds");
            ImGui::Checkbox("Enabled", &clouds.enabled);
            constexpr ImGuiSliderFlags kLog = ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp;
            DragFloatInRange("Coverage", &clouds.coverage, 0.0f, 1.0f, "%.2f");
            ImGui::DragFloat("Base altitude (m)", &clouds.baseAltitude, 10.0f, 100.0f, 10000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::DragFloat("Thickness (m)", &clouds.thickness, 10.0f, 100.0f, 10000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::DragFloat("Density (/m)", &clouds.density, 0.001f, 0.001f, 0.5f, "%.4f", kLog);
            ImGui::DragFloat("Shape scale (m)", &clouds.shapeScale, 10.0f, 500.0f, 100000.0f, "%.0f", kLog);
            ImGui::DragFloat("Billow scale (m)", &clouds.detailScale, 1.0f, 50.0f, 10000.0f, "%.0f", kLog);
            ImGui::DragFloat("Plume map scale (m)", &clouds.weatherScale, 10.0f, 1000.0f, 500000.0f, "%.0f", kLog);
            DragFloatInRange("Billows", &clouds.billows, 0.0f, 2.0f, "%.2f");
            DragFloatInRange("Forward anisotropy", &clouds.forwardAnisotropy, 0.0f, 0.95f, "%.2f");
            DragFloatInRange("Back anisotropy", &clouds.backAnisotropy, -0.95f, 0.0f, "%.2f");
            DragFloatInRange("Back weight", &clouds.backWeight, 0.0f, 1.0f, "%.2f");
            DragFloatInRange("Albedo", &clouds.albedo, 0.0f, 1.0f, "%.3f");
            DragFloatInRange("Ambient scale", &clouds.ambientScale, 0.0f, 4.0f, "%.2f");
            ImGui::DragFloat("Haze distance (m)", &clouds.hazeDistance, 100.0f, 1000.0f, 1000000.0f, "%.0f", kLog);
            DragFloatInRange("Diffusion", &clouds.diffusion, 0.0f, 1.0f, "%.2f");
            DragFloatInRange("Ambient occlusion", &clouds.ambientOcclusion, 0.0f, 1.0f, "%.2f");
            ImGui::SeparatorText("Motion");
            ImGui::DragFloat("Updraft (m/s)", &clouds.updraft, 0.05f, 0.0f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("How fast the billows rise through the plumes. The wind (above) carries the layer.");
            }
            ImGui::DragFloat("Lifetime (min)", &clouds.lifetime, 0.5f, 1.0f, 240.0f, "%.1f", kLog);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("A mid-sized plume's life: it rises out of the base, stands and dissipates.\nThe large clusters live twice as long, the smallest plumes 0.7x.");
            }
            ImGui::DragFloat("Time scale##clouds", &clouds.timeScale, 0.1f, 0.0f, 3600.0f, "%.1fx", kLog);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("The clouds' seconds per real second; 0 holds them.");
            }
            ImGui::PopID();
        }
    }
    else if (environment.mode == EnvironmentMode::Hdri)
    {
        HdriSettings& hdri = environment.hdri;
        ImGui::TextWrapped("%s", hdri.path.empty() ? "<no HDRI>" : hdri.path.c_str());
        if (const std::optional<std::string> path =
                PickFilePath(FileDialogType::OpenTexture, ImGui::Button("Choose HDRI..."));
            path.has_value())
        {
            hdri.path = *path;
            hdri.uuid = AssetRegistry::GetOrCreateUuid(*path);
        }
        ImGui::DragFloat("Intensity (cd/m2)", &hdri.intensity, 10.0f, 0.0f, 1000000.0f, "%.0f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
        DragFloatInRange("Rotation (deg)", &hdri.rotationDegrees, -180.0f, 180.0f, "%.1f", 0.5f);
    }

    if (!(environment == scene.GetEnvironment()))
    {
        scene.SetEnvironment(environment);
    }
}

void DrawGizmoControls(GizmoSettings& gizmo)
{
    DrawOperationButton("Combined", kCombinedGizmoOperation, gizmo.operation);
    ImGui::SameLine();
    DrawOperationButton("Rotate", ImGuizmo::ROTATE, gizmo.operation);
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
        "%.2f",
        ImGuiSliderFlags_AlwaysClamp);
    ImGui::DragFloat("Rotate Snap", &gizmo.rotationSnap, 0.5f, 1.0f, 90.0f, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
    ImGui::DragFloat3("Scale Snap", glm::value_ptr(gizmo.scaleSnap), 0.01f, 0.01f, WorldUnits::kUiScaleSnapMax, "%.2f", ImGuiSliderFlags_AlwaysClamp);
}

void DrawLightComponentEditor(LightComponent& light)
{
    // Type selector
    const char* currentTypeLabel = GetLightTypeLabel(light.type);
    if (ImGui::BeginCombo("Type", currentTypeLabel))
    {
        constexpr LightType kTypes[] = {
            LightType::Directional, LightType::Point, LightType::Spot,
            LightType::Area, LightType::Ambient, LightType::Hemisphere};
        for (LightType t : kTypes)
        {
            const bool selected = t == light.type;
            if (ImGui::Selectable(GetLightTypeLabel(t), selected))
            {
                light.type = t;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // Color; a hemisphere light's is its sky's, and it has a ground colour below the horizon.
    ImGui::ColorEdit3(light.type == LightType::Hemisphere ? "Sky Color" : "Color", &light.color.x, ImGuiColorEditFlags_Float);
    if (light.type == LightType::Hemisphere)
    {
        ImGui::ColorEdit3("Ground Color", &light.groundColor.x, ImGuiColorEditFlags_Float);
        ImGui::TextDisabled("Sky above the light's up axis (rotate it to tilt)");
    }

    // Intensity with unit label
    const char* intensityUnit = "lm"; // lumens for most lights
    if (light.type == LightType::Directional)
        intensityUnit = "lx";
    if (light.type == LightType::Ambient || light.type == LightType::Hemisphere)
        intensityUnit = "cd/m^2";

    const std::string intensityLabel = std::string("Intensity (") + intensityUnit + ")";
    ImGui::DragFloat(intensityLabel.c_str(), &light.intensity, 10.0f, 0.0f, 1000000.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    light.intensity = std::max(light.intensity, 0.0f);

    // Range
    if (light.type != LightType::Directional && light.type != LightType::Ambient && light.type != LightType::Hemisphere)
    {
        ImGui::DragFloat("Range (m)", &light.range, 0.1f, 0.1f, 1000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        light.range = std::max(light.range, 0.01f);
        // A slot in the local shadow atlas; lights past its capacity cast none (see the log).
        ImGui::Checkbox("Cast Shadows", &light.castShadows);
    }

    // The size of the emitting sphere, which widens and dims the highlight on smooth surfaces.
    if (light.type == LightType::Point || light.type == LightType::Spot)
    {
        ImGui::DragFloat("Source Radius (m)", &light.sourceRadius, 0.005f, 0.0f, 10.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        light.sourceRadius = std::max(light.sourceRadius, 0.0f);
    }

    // Spot angles
    if (light.type == LightType::Spot)
    {
        DragFloatInRange("Inner Angle", &light.spotInnerAngleDegrees, 1.0f, 89.0f, "%.1f deg", 0.25f);
        DragFloatInRange("Outer Angle", &light.spotOuterAngleDegrees, 1.0f, 89.0f, "%.1f deg", 0.25f);
        light.spotInnerAngleDegrees = std::clamp(light.spotInnerAngleDegrees, 1.0f, 89.0f);
        light.spotOuterAngleDegrees = std::clamp(light.spotOuterAngleDegrees,
                                                 light.spotInnerAngleDegrees, 89.0f);
    }

    // Area size
    if (light.type == LightType::Area)
    {
        ImGui::DragFloat2("Area Size (m)", &light.areaSize.x, 0.05f, 0.01f, 100.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        light.areaSize = glm::max(light.areaSize, glm::vec2(0.01f));
        ImGui::TextDisabled("One-sided: emits along the gizmo's arrow (local -Z); W along local X, H along local Y.");
    }

    // Tip
    ImGui::Spacing();
    ImGui::TextDisabled("Tip: use the viewport gizmo to move/rotate this light.");
}
}

ScenePanel::ScenePanel()
    : EditorPanel("scene", "Scene", ICON_PH_TREE_STRUCTURE, EditorDockSlot::Left)
{
    Open();
}

void ScenePanel::OnGui(EditorContext& context)
{
    IEditorWorld& scene = context.scene;
    EditorUiFrameResult& result = context.result;
    const std::string& lastLoadError = context.frame.lastLoadError;
    const std::string& lastSceneIoError = context.frame.lastSceneIoError;
    const bool sceneUnsaved = context.state.sceneUnsaved;
    const std::string& sceneUploadStatus = context.frame.sceneUploadStatus;

    // Model loads and uploads fail without interrupting the editor, so this line is the only
    // place the user learns that the scene on screen is not the one they asked for.
    if (!lastLoadError.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ui_colors::kTextDanger);
        ImGui::TextWrapped("Error: %s", lastLoadError.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    // Streamed cells come and go with the focus: they are counted, not listed.
    const size_t streamedCount = scene.Registry().view<const StreamedComponent>().size();
    const size_t modelCount = scene.Registry().view<const ModelComponent>().size() - streamedCount;
    const size_t lightCount = scene.Registry().view<const LightComponent>().size();
    if (streamedCount > 0)
    {
        ImGui::Text("Models: %u  Lights: %u  Streamed cells: %u",
                    static_cast<unsigned int>(modelCount),
                    static_cast<unsigned int>(lightCount),
                    static_cast<unsigned int>(streamedCount));
    }
    else
    {
        ImGui::Text("Models: %u  Lights: %u",
                    static_cast<unsigned int>(modelCount),
                    static_cast<unsigned int>(lightCount));
    }
    // Texture files prepare in the background; the scene on screen changes once they are ready.
    // The status shares the counts line: a line of its own would come and go with every scene
    // update and push the whole panel down and back up.
    if (!sceneUploadStatus.empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", sceneUploadStatus.c_str());
    }

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
                    type};
            }
        };
        addLight(LightType::Point, "Point");
        addLight(LightType::Directional, "Directional");
        addLight(LightType::Spot, "Spot");
        addLight(LightType::Area, "Area");
        addLight(LightType::Ambient, "Ambient");
        addLight(LightType::Hemisphere, "Hemisphere");
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
        if (!scene.HasModelComponent(entity) || scene.Registry().all_of<StreamedComponent>(entity))
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
        if (modelCount != 0u)
            ImGui::Spacing();
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
        const std::string label = std::string("[") + GetLightTypeLabel(light.type)[0] + "] " +
                                  tag.name + "##light_" +
                                  std::to_string(static_cast<uint32_t>(entt::to_integral(entity)));
        ImGui::PushStyleColor(ImGuiCol_Text, typeColor);
        const bool clicked = ImGui::Selectable(label.c_str(), scene.IsSelected(entity));
        ImGui::PopStyleColor();
        if (clicked)
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

        ImGui::InputText("Name", &tag.name);
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
                DrawLightComponentEditor(light);
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
            GizmoSettings& gizmo = scene.GetGizmoSettings();

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

                if (!metadata.materialVariants.empty())
                {
                    const char* preview = model.materialVariant.empty() ? "Default" : model.materialVariant.c_str();
                    if (ImGui::BeginCombo("Material Variant", preview))
                    {
                        if (ImGui::Selectable("Default", model.materialVariant.empty()) && !model.materialVariant.empty())
                        {
                            result.actions.selectedMaterialVariant = std::string{};
                        }
                        for (size_t index = 0; index < metadata.materialVariants.size(); ++index)
                        {
                            const std::string& name = metadata.materialVariants[index];
                            const std::string label = name + "##variant_" + std::to_string(index);
                            if (ImGui::Selectable(label.c_str(), name == model.materialVariant) && name != model.materialVariant)
                            {
                                result.actions.selectedMaterialVariant = name;
                            }
                        }
                        ImGui::EndCombo();
                    }
                }

                DrawModelAnimationControls(model, result);
                DrawModelDriverControls(scene, selectedEntity, model, context.state, result);

                ImGui::BeginDisabled(model.sourcePath.empty());
                if (ImGui::Button("Edit Materials"))
                {
                    context.windows.Get<ModelProcessorWindow>().OpenModel(context, model.sourcePath, true);
                }
                ImGui::SetItemTooltip("Opens the model's materials in Model Preview, its paint selected; edits show here as they are made.");
                ImGui::EndDisabled();

                if (metadata.modelLightCount > 0)
                {
                    bool useModelLights = model.useModelLights;
                    const std::string label = "Model Lights (" + std::to_string(metadata.modelLightCount) + ")";
                    if (ImGui::Checkbox(label.c_str(), &useModelLights))
                    {
                        result.actions.selectedUseModelLights = useModelLights;
                    }
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

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawEnvironmentEditor(scene);
    }

    // Scene I/O — always visible
    ImGui::Separator();
    // Saving is File > Save Scene (Ctrl+S) and Save Scene As.
    ImGui::TextWrapped("Scene: %s", scene.GetSceneFilePath().empty() ? "<unsaved>" : scene.GetSceneFilePath().c_str());
    if (sceneUnsaved)
    {
        ImGui::TextColored(ui_colors::kTextWarning, "Unsaved changes (File > Save Scene, Ctrl+S)");
    }
    if (!lastSceneIoError.empty())
    {
        ImGui::TextColored(ui_colors::kTextDanger, "Error: %s", lastSceneIoError.c_str());
    }
    if (const std::optional<std::string> loadPath =
            PickFilePath(FileDialogType::OpenScene, ImGui::Button("Load Scene"));
        loadPath.has_value())
    {
        result.actions.selectedSceneLoadPath = *loadPath;
    }
}
}
