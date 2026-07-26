#include "editor_scene.h"

#include <log/log.h>
#include <uuid/uuid.h>
#include <yaml-cpp/yaml.h>

#define GLM_ENABLE_EXPERIMENTAL
#define GLM_FORCE_RADIANS
#include <glm/common.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace
{
const char* LightTypeToString(LightType type)
{
    switch (type)
    {
    case LightType::Directional: return "directional";
    case LightType::Point:       return "point";
    case LightType::Spot:        return "spot";
    case LightType::Area:        return "area";
    case LightType::Ambient:     return "ambient";
    default:                     return "point";
    }
}

LightType LightTypeFromString(const std::string& value)
{
    if (value == "directional") return LightType::Directional;
    if (value == "spot")        return LightType::Spot;
    if (value == "area")        return LightType::Area;
    if (value == "ambient")     return LightType::Ambient;
    return LightType::Point;
}

glm::vec3 ReadVec3(const YAML::Node& node, const glm::vec3& fallback)
{
    if (!node || !node.IsSequence() || node.size() != 3)
    {
        return fallback;
    }

    return glm::vec3(node[0].as<float>(), node[1].as<float>(), node[2].as<float>());
}

std::string SanitizeName(const std::string& value, const std::string& fallback)
{
    return value.empty() ? fallback : value;
}

ImGuizmo::OPERATION ParseOperation(
    const std::string& value,
    ImGuizmo::OPERATION fallback
)
{
    if (value == "scale")
    {
        return ImGuizmo::SCALE;
    }
    if (value == "combined" || value == "translate" || value == "rotate")
    {
        return kCombinedGizmoOperation;
    }
    return fallback;
}

ImGuizmo::MODE ParseMode(const std::string& value)
{
    if (value == "local")
    {
        return ImGuizmo::LOCAL;
    }
    return ImGuizmo::WORLD;
}

const char* ToString(ImGuizmo::OPERATION operation)
{
    return operation == ImGuizmo::SCALE ? "scale" : "combined";
}

const char* ToString(ImGuizmo::MODE mode)
{
    return mode == ImGuizmo::LOCAL ? "local" : "world";
}

TransformComponent ReadTransformComponent(const YAML::Node& transformNode, const TransformComponent& fallback)
{
    TransformComponent transform = fallback;
    transform.translation = ReadVec3(transformNode["translation"], transform.translation);
    transform.rotationDegrees = ReadVec3(transformNode["rotation"], transform.rotationDegrees);
    transform.scale = glm::max(ReadVec3(transformNode["scale"], transform.scale), WorldUnits::kMinimumScale3);
    return transform;
}

GizmoSettings ReadGizmoSettings(const YAML::Node& gizmoNode, const GizmoSettings& fallback)
{
    GizmoSettings settings = fallback;
    settings.operation = ParseOperation(
        gizmoNode["operation"].as<std::string>(ToString(settings.operation)),
        settings.operation
    );
    settings.mode = ParseMode(gizmoNode["mode"].as<std::string>(ToString(settings.mode)));
    settings.useSnap = gizmoNode["use_snap"].as<bool>(settings.useSnap);
    settings.translationSnap = ReadVec3(gizmoNode["translation_snap"], settings.translationSnap);
    settings.rotationSnap = gizmoNode["rotation_snap"].as<float>(settings.rotationSnap);
    settings.scaleSnap = ReadVec3(gizmoNode["scale_snap"], settings.scaleSnap);
    return settings;
}

void EmitVec3(YAML::Emitter& emitter, const char* key, const glm::vec3& value)
{
    emitter << YAML::Key << key << YAML::Value << YAML::Flow << YAML::BeginSeq
            << value.x << value.y << value.z << YAML::EndSeq;
}

float UnwrapDegrees(float value, float reference)
{
    return reference + std::remainder(value - reference, 360.0f);
}

glm::vec3 UnwrapRotationDegrees(const glm::vec3& value, const glm::vec3& reference)
{
    return glm::vec3(
        UnwrapDegrees(value.x, reference.x),
        UnwrapDegrees(value.y, reference.y),
        UnwrapDegrees(value.z, reference.z)
    );
}

TransformComponent DecomposeTransformMatrix(const glm::mat4& matrix, const TransformComponent& reference)
{
    glm::vec3 scale{};
    glm::quat orientation{};
    glm::vec3 translation{};
    glm::vec3 skew{};
    glm::vec4 perspective{};
    if (!glm::decompose(matrix, scale, orientation, translation, skew, perspective))
    {
        TransformComponent fallback = reference;
        fallback.translation = glm::vec3(matrix[3]);
        return fallback;
    }

    orientation = glm::normalize(orientation);
    glm::mat4 rotationMatrix = glm::mat4_cast(orientation);
    float rotationX = 0.0f;
    float rotationY = 0.0f;
    float rotationZ = 0.0f;
    glm::extractEulerAngleXYZ(rotationMatrix, rotationX, rotationY, rotationZ);

    TransformComponent transform{};
    transform.translation = translation;
    transform.rotationDegrees = UnwrapRotationDegrees(
        glm::degrees(glm::vec3(rotationX, rotationY, rotationZ)),
        reference.rotationDegrees
    );
    transform.scale = glm::max(glm::abs(scale), WorldUnits::kMinimumScale3);
    return transform;
}

SerializedSceneData ReadSceneData(const YAML::Node& root)
{
    SerializedSceneData sceneData{};
    sceneData.gizmo = ReadGizmoSettings(root["editor"]["gizmo"], sceneData.gizmo);

    const YAML::Node entitiesNode = root["entities"];
    if (entitiesNode && entitiesNode.IsSequence())
    {
        for (const YAML::Node& entityNode : entitiesNode)
        {
            SerializedEntityData entityData{};
            entityData.entityUuid = entityNode["entity_uuid"].as<std::string>(entityData.entityUuid);
            entityData.tagName = entityNode["tag"].as<std::string>(entityData.tagName);

            const YAML::Node modelNode = entityNode["model"];
            entityData.modelDisplayName = modelNode["display_name"].as<std::string>(entityData.modelDisplayName);
            entityData.modelSourcePath = modelNode["source_path"].as<std::string>(entityData.modelSourcePath);
            entityData.modelSourceUuid = modelNode["source_uuid"].as<std::string>(entityData.modelSourceUuid);
            entityData.modelBaseColorTextureOverridePath =
                modelNode["base_color_texture_override"].as<std::string>(entityData.modelBaseColorTextureOverridePath);
            entityData.modelBaseColorTextureOverrideUuid =
                modelNode["base_color_texture_override_uuid"].as<std::string>(entityData.modelBaseColorTextureOverrideUuid);
            entityData.transform = ReadTransformComponent(entityNode["transform"], entityData.transform);
            sceneData.entities.push_back(entityData);
        }
    }

    const YAML::Node lightsNode = root["lights"];
    if (lightsNode && lightsNode.IsSequence())
    {
        for (const YAML::Node& lightNode : lightsNode)
        {
            SerializedLightData lightData{};
            lightData.entityUuid = lightNode["entity_uuid"].as<std::string>(lightData.entityUuid);
            lightData.tagName = lightNode["tag"].as<std::string>(lightData.tagName);
            lightData.lightType = LightTypeFromString(lightNode["light_type"].as<std::string>("point"));
            lightData.color = ReadVec3(lightNode["color"], lightData.color);
            lightData.intensity = lightNode["intensity"].as<float>(lightData.intensity);
            lightData.range = lightNode["range"].as<float>(lightData.range);
            lightData.spotInnerAngle = lightNode["spot_inner_angle"].as<float>(lightData.spotInnerAngle);
            lightData.spotOuterAngle = lightNode["spot_outer_angle"].as<float>(lightData.spotOuterAngle);
            if (lightNode["area_size"] && lightNode["area_size"].IsSequence() && lightNode["area_size"].size() == 2)
            {
                lightData.areaSize.x = lightNode["area_size"][0].as<float>(lightData.areaSize.x);
                lightData.areaSize.y = lightNode["area_size"][1].as<float>(lightData.areaSize.y);
            }
            lightData.transform = ReadTransformComponent(lightNode["transform"], lightData.transform);
            sceneData.lights.push_back(lightData);
        }
    }

    sceneData.selectedEntityUuid = root["scene"]["selected_entity_uuid"].as<std::string>(sceneData.selectedEntityUuid);
    sceneData.selectedEntityIndex = root["scene"]["selected_entity"].as<int>(0);
    return sceneData;
}

std::string EmitSceneYaml(const SerializedSceneData& sceneData)
{
    YAML::Emitter emitter;
    emitter << YAML::BeginMap;
    emitter << YAML::Key << "scene" << YAML::Value << YAML::BeginMap;
    emitter << YAML::Key << "version" << YAML::Value << 3;
    emitter << YAML::Key << "selected_entity_uuid" << YAML::Value << sceneData.selectedEntityUuid;
    emitter << YAML::Key << "selected_entity" << YAML::Value << sceneData.selectedEntityIndex;
    emitter << YAML::EndMap;

    emitter << YAML::Key << "entities" << YAML::Value << YAML::BeginSeq;
    for (const SerializedEntityData& entity : sceneData.entities)
    {
        emitter << YAML::BeginMap;
        emitter << YAML::Key << "entity_uuid" << YAML::Value << entity.entityUuid;
        emitter << YAML::Key << "tag" << YAML::Value << entity.tagName;
        emitter << YAML::Key << "model" << YAML::Value << YAML::BeginMap;
        emitter << YAML::Key << "display_name" << YAML::Value << entity.modelDisplayName;
        emitter << YAML::Key << "source_path" << YAML::Value << entity.modelSourcePath;
        emitter << YAML::Key << "source_uuid" << YAML::Value << entity.modelSourceUuid;
        emitter << YAML::Key << "base_color_texture_override" << YAML::Value << entity.modelBaseColorTextureOverridePath;
        emitter << YAML::Key << "base_color_texture_override_uuid" << YAML::Value << entity.modelBaseColorTextureOverrideUuid;
        emitter << YAML::EndMap;
        emitter << YAML::Key << "transform" << YAML::Value << YAML::BeginMap;
        EmitVec3(emitter, "translation", entity.transform.translation);
        EmitVec3(emitter, "rotation", entity.transform.rotationDegrees);
        EmitVec3(emitter, "scale", entity.transform.scale);
        emitter << YAML::EndMap;
        emitter << YAML::EndMap;
    }
    emitter << YAML::EndSeq;

    emitter << YAML::Key << "lights" << YAML::Value << YAML::BeginSeq;
    for (const SerializedLightData& light : sceneData.lights)
    {
        emitter << YAML::BeginMap;
        emitter << YAML::Key << "entity_uuid" << YAML::Value << light.entityUuid;
        emitter << YAML::Key << "tag" << YAML::Value << light.tagName;
        emitter << YAML::Key << "light_type" << YAML::Value << LightTypeToString(light.lightType);
        EmitVec3(emitter, "color", light.color);
        emitter << YAML::Key << "intensity" << YAML::Value << light.intensity;
        emitter << YAML::Key << "range" << YAML::Value << light.range;
        emitter << YAML::Key << "spot_inner_angle" << YAML::Value << light.spotInnerAngle;
        emitter << YAML::Key << "spot_outer_angle" << YAML::Value << light.spotOuterAngle;
        emitter << YAML::Key << "area_size" << YAML::Value << YAML::Flow << YAML::BeginSeq
                << light.areaSize.x << light.areaSize.y << YAML::EndSeq;
        emitter << YAML::Key << "transform" << YAML::Value << YAML::BeginMap;
        EmitVec3(emitter, "translation", light.transform.translation);
        EmitVec3(emitter, "rotation", light.transform.rotationDegrees);
        EmitVec3(emitter, "scale", light.transform.scale);
        emitter << YAML::EndMap;
        emitter << YAML::EndMap;
    }
    emitter << YAML::EndSeq;

    emitter << YAML::Key << "editor" << YAML::Value << YAML::BeginMap;
    emitter << YAML::Key << "gizmo" << YAML::Value << YAML::BeginMap;
    emitter << YAML::Key << "operation" << YAML::Value << ToString(sceneData.gizmo.operation);
    emitter << YAML::Key << "mode" << YAML::Value << ToString(sceneData.gizmo.mode);
    emitter << YAML::Key << "use_snap" << YAML::Value << sceneData.gizmo.useSnap;
    EmitVec3(emitter, "translation_snap", sceneData.gizmo.translationSnap);
    emitter << YAML::Key << "rotation_snap" << YAML::Value << sceneData.gizmo.rotationSnap;
    EmitVec3(emitter, "scale_snap", sceneData.gizmo.scaleSnap);
    emitter << YAML::EndMap;
    emitter << YAML::EndMap;
    emitter << YAML::EndMap;

    return emitter.c_str();
}
}

std::unique_ptr<IEditorWorld> CreateEditorWorld()
{
    return std::make_unique<EditorScene>();
}

SerializedSceneData LoadEditorSceneDataFromFile(const std::string& path)
{
    return ReadSceneData(YAML::LoadFile(path));
}

void SaveEditorSceneDataToFile(const SerializedSceneData& sceneData, const std::string& path)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open())
    {
        throw std::runtime_error("Failed to open scene file for writing: " + path);
    }

    output << EmitSceneYaml(sceneData);
    if (!output.good())
    {
        throw std::runtime_error("Failed to write scene file: " + path);
    }
}

EditorScene::EditorScene()
{
    // Every scene entity carries a TagComponent, so its on_destroy signal fires
    // for any destruction path (DestroyEntity, Clear, future systems) and keeps
    // the scene order and selection in sync without per-call-site bookkeeping.
    m_registry.on_destroy<TagComponent>().connect<&EditorScene::OnEntityDestroyed>(this);
    m_registry.on_destroy<SceneEntityIdComponent>().connect<&EditorScene::OnSceneEntityIdDestroyed>(this);
}

void EditorScene::LoadConfig(const std::string& path)
{
    m_configPath = path;

    if (!std::filesystem::exists(path))
    {
        return;
    }

    const YAML::Node root = YAML::LoadFile(path);
    m_defaultTransform = ReadTransformComponent(root["entity"]["transform"], m_defaultTransform);
    m_gizmoSettings = ReadGizmoSettings(root["editor"]["gizmo"], m_gizmoSettings);
}

void EditorScene::SetSceneFilePath(const std::string& path)
{
    m_sceneFilePath = path;
}

void EditorScene::CreateTwoCubeTestScene()
{
    Clear();

    SerializedEntityData leftCube{};
    leftCube.tagName = "Cube A";
    leftCube.modelDisplayName = "Cube A";
    leftCube.transform.translation = glm::vec3(-1.25f, 0.0f, 0.0f);

    SerializedEntityData rightCube{};
    rightCube.tagName = "Cube B";
    rightCube.modelDisplayName = "Cube B";
    rightCube.transform.translation = glm::vec3(1.25f, 0.0f, 0.0f);

    CreateEntity(leftCube);
    CreateEntity(rightCube);
    EnsureSelection();
}

void EditorScene::Clear()
{
    // Clear editor-side state first so on_destroy callbacks remain O(1) while
    // registry.clear() destroys every component pool entry.
    m_sceneOrder.clear();
    m_entityByUuid.clear();
    m_selectedEntity = entt::null;
    m_registry.clear();
}

entt::entity EditorScene::CreateEntity(const SerializedEntityData& entityData)
{
    entt::entity entity = m_registry.create();
    const std::string entityUuid = AdoptOrCreateEntityUuid(entityData.entityUuid);
    m_registry.emplace<SceneEntityIdComponent>(entity, SceneEntityIdComponent{ entityUuid });
    m_entityByUuid.emplace(entityUuid, entity);
    m_registry.emplace<TagComponent>(entity, TagComponent{ entityData.tagName });
    m_registry.emplace<TransformComponent>(entity, entityData.transform);
    m_registry.emplace<WorldTransformComponent>(entity, WorldTransformComponent{ BuildTransformMatrix(entityData.transform) });
    ModelComponent& model = m_registry.emplace<ModelComponent>(entity);
    model.sourcePath = entityData.modelSourcePath;
    model.displayName = entityData.modelDisplayName;
    model.baseColorTextureOverridePath = entityData.modelBaseColorTextureOverridePath;
    model.sourceUuid = entityData.modelSourceUuid;
    model.baseColorTextureOverrideUuid = entityData.modelBaseColorTextureOverrideUuid;
    m_registry.emplace<ModelBoundsComponent>(entity);
    m_registry.emplace<EditorModelMetadataComponent>(entity);
    m_registry.emplace<ModelRenderableDirty>(entity);
    m_sceneOrder.push_back(entity);
    if (m_selectedEntity == entt::null)
    {
        m_selectedEntity = entity;
    }
    return entity;
}

void EditorScene::DestroyEntity(entt::entity entity)
{
    if (!IsValidEntity(entity))
    {
        return;
    }

    m_registry.destroy(entity);
}

void EditorScene::OnEntityDestroyed(entt::registry&, entt::entity entity)
{
    m_sceneOrder.erase(
        std::remove(m_sceneOrder.begin(), m_sceneOrder.end(), entity),
        m_sceneOrder.end()
    );

    if (m_selectedEntity == entity)
    {
        m_selectedEntity = entt::null;
        EnsureSelection();
    }
}

bool EditorScene::HasEntities() const
{
    return !m_sceneOrder.empty();
}

bool EditorScene::HasSelection() const
{
    return IsValidEntity(m_selectedEntity);
}

bool EditorScene::IsSelected(entt::entity entity) const
{
    return IsValidEntity(entity) && entity == m_selectedEntity;
}

entt::entity EditorScene::GetSelectedEntity() const
{
    return m_selectedEntity;
}

void EditorScene::SetSelectedEntity(entt::entity entity)
{
    m_selectedEntity = IsValidEntity(entity) ? entity : entt::null;
}

void EditorScene::ClearSelection()
{
    m_selectedEntity = entt::null;
}

const std::vector<entt::entity>& EditorScene::GetSceneOrder() const
{
    return m_sceneOrder;
}

TagComponent& EditorScene::EditTag(entt::entity entity)
{
    return m_registry.get<TagComponent>(entity);
}

TransformComponent& EditorScene::EditTransform(entt::entity entity)
{
    return m_registry.get<TransformComponent>(entity);
}

ModelComponent& EditorScene::EditModel(entt::entity entity)
{
    return m_registry.get<ModelComponent>(entity);
}

LightComponent& EditorScene::EditLightComponent(entt::entity entity)
{
    return m_registry.get<LightComponent>(entity);
}

void EditorScene::MarkTransformDirty(entt::entity entity)
{
    if (IsValidEntity(entity) && m_registry.all_of<TransformComponent>(entity))
    {
        m_registry.emplace_or_replace<TransformDirty>(entity);
    }
}

void EditorScene::OnSceneEntityIdDestroyed(entt::registry& registry, entt::entity entity)
{
    const std::string& uuid = registry.get<SceneEntityIdComponent>(entity).value;
    if (const auto it = m_entityByUuid.find(uuid); it != m_entityByUuid.end() && it->second == entity)
    {
        m_entityByUuid.erase(it);
    }
}

std::string EditorScene::AdoptOrCreateEntityUuid(const std::string& requestedUuid)
{
    if (!requestedUuid.empty() && !m_entityByUuid.contains(requestedUuid))
    {
        return requestedUuid;
    }

    std::string uuid;
    do
    {
        uuid = Uuid::GenerateV4();
    }
    while (m_entityByUuid.contains(uuid));

    if (!requestedUuid.empty())
    {
        LOG_WARN("Duplicate scene entity uuid {}; assigned replacement {}", requestedUuid, uuid);
    }
    return uuid;
}

void EditorScene::MarkModelRenderableDirty(entt::entity entity)
{
    if (HasModelComponent(entity))
    {
        m_registry.emplace_or_replace<ModelRenderableDirty>(entity);
    }
}

void EditorScene::ClearModelRenderableDirty(entt::entity entity)
{
    if (IsValidEntity(entity))
    {
        m_registry.remove<ModelRenderableDirty>(entity);
    }
}

void EditorScene::ClearAllModelRenderableDirty()
{
    m_registry.clear<ModelRenderableDirty>();
}

std::vector<entt::entity> EditorScene::GetDirtyModelRenderableEntities() const
{
    std::vector<entt::entity> entities;
    const auto dirty = m_registry.view<const ModelComponent, const ModelRenderableDirty>();
    entities.reserve(dirty.size_hint());
    for (entt::entity entity : dirty)
    {
        entities.push_back(entity);
    }
    return entities;
}

GizmoSettings& EditorScene::GetGizmoSettings()
{
    return m_gizmoSettings;
}

const GizmoSettings& EditorScene::GetGizmoSettings() const
{
    return m_gizmoSettings;
}

void EditorScene::ResetSelectedTransform()
{
    if (!HasSelection())
    {
        return;
    }

    EditSelectedTransform() = m_defaultTransform;
    MarkTransformDirty(m_selectedEntity);
}

void EditorScene::UpdateModelInfo(
    entt::entity entity,
    const std::string& displayName,
    const std::string& sourcePath,
    uint32_t submeshCount,
    const glm::vec3& minBounds,
    const glm::vec3& maxBounds,
    bool hasBounds,
    const std::vector<ModelImportedMaterialInfo>& importedMaterials,
    const std::vector<ModelImportedSubmeshInfo>& importedSubmeshes
)
{
    auto [model, bounds, metadata, tag] = m_registry.get<
        ModelComponent,
        ModelBoundsComponent,
        EditorModelMetadataComponent,
        TagComponent
    >(entity);
    model.displayName = SanitizeName(displayName, model.displayName);
    model.sourcePath = sourcePath;
    bounds.minBounds = minBounds;
    bounds.maxBounds = maxBounds;
    bounds.hasBounds = hasBounds;
    metadata.submeshCount = std::max(submeshCount, 1u);
    metadata.importedMaterials = importedMaterials;
    metadata.importedSubmeshes = importedSubmeshes;

    if (sourcePath.empty())
    {
        tag.name = model.displayName;
    }
    else
    {
        const std::filesystem::path modelPath(sourcePath);
        tag.name = SanitizeName(modelPath.stem().string(), model.displayName);
    }
}

glm::mat4 EditorScene::GetModelMatrix(entt::entity entity) const
{
    if (m_registry.all_of<TransformDirty>(entity))
    {
        return BuildTransformMatrix(GetTransform(entity));
    }
    return m_registry.get<WorldTransformComponent>(entity).matrix;
}

void EditorScene::ApplyTransformMatrix(entt::entity entity, const glm::mat4& matrix)
{
    TransformComponent& transform = EditTransform(entity);
    transform = DecomposeTransformMatrix(matrix, transform);
    MarkTransformDirty(entity);
}

glm::vec3 EditorScene::GetBoundsCenter(entt::entity entity) const
{
    const ModelBoundsComponent* bounds = m_registry.try_get<ModelBoundsComponent>(entity);
    if (bounds == nullptr)
    {
        return glm::vec3(0.0f);
    }
    return (bounds->minBounds + bounds->maxBounds) * 0.5f;
}

void EditorScene::ApplySceneData(const SerializedSceneData& sceneData)
{
    Clear();
    m_gizmoSettings = sceneData.gizmo;

    for (const SerializedEntityData& entityData : sceneData.entities)
    {
        CreateEntity(entityData);
    }

    for (const SerializedLightData& lightData : sceneData.lights)
    {
        CreateLightEntity(lightData);
    }

    m_selectedEntity = entt::null;
    if (!sceneData.selectedEntityUuid.empty())
    {
        if (const auto it = m_entityByUuid.find(sceneData.selectedEntityUuid); it != m_entityByUuid.end())
        {
            m_selectedEntity = it->second;
        }
    }

    const size_t modelCount = m_registry.view<const ModelComponent>().size();
    if (m_selectedEntity == entt::null && modelCount != 0u)
    {
        const size_t selectedModelIndex = static_cast<size_t>(std::clamp(
            sceneData.selectedEntityIndex,
            0,
            static_cast<int>(modelCount) - 1
        ));
        size_t modelIndex = 0;
        for (entt::entity entity : m_sceneOrder)
        {
            if (!m_registry.all_of<ModelComponent>(entity))
            {
                continue;
            }
            if (modelIndex == selectedModelIndex)
            {
                m_selectedEntity = entity;
                break;
            }
            ++modelIndex;
        }
    }
    EnsureSelection();
}

void EditorScene::SaveSceneToFile(const std::string& path) const
{
    SaveEditorSceneDataToFile(CaptureSceneData(), path);
}

std::string EditorScene::BuildSceneYamlPreview() const
{
    return EmitSceneYaml(CaptureSceneData());
}

const std::string& EditorScene::GetConfigPath() const
{
    return m_configPath;
}

const std::string& EditorScene::GetSceneFilePath() const
{
    return m_sceneFilePath;
}

void EditorScene::FlushDirtyTransforms()
{
    auto dirtyTransforms = m_registry.view<TransformComponent, WorldTransformComponent, TransformDirty>();
    for (entt::entity entity : dirtyTransforms)
    {
        auto [transform, worldTransform] =
            m_registry.get<TransformComponent, WorldTransformComponent>(entity);
        worldTransform.matrix = BuildTransformMatrix(transform);
    }
    m_registry.clear<TransformDirty>();
}

bool EditorScene::IsValidEntity(entt::entity entity) const
{
    return m_registry.valid(entity);
}

void EditorScene::EnsureSelection()
{
    if (HasSelection())
    {
        return;
    }

    if (!m_sceneOrder.empty())
    {
        m_selectedEntity = m_sceneOrder.front();
    }
}

entt::entity EditorScene::CreateLightEntity(const SerializedLightData& lightData)
{
    entt::entity entity = m_registry.create();
    const std::string entityUuid = AdoptOrCreateEntityUuid(lightData.entityUuid);
    m_registry.emplace<SceneEntityIdComponent>(entity, SceneEntityIdComponent{ entityUuid });
    m_entityByUuid.emplace(entityUuid, entity);
    m_registry.emplace<TagComponent>(entity, TagComponent{ lightData.tagName });
    m_registry.emplace<TransformComponent>(entity, lightData.transform);
    m_registry.emplace<WorldTransformComponent>(entity, WorldTransformComponent{ BuildTransformMatrix(lightData.transform) });
    LightComponent light{};
    light.type = lightData.lightType;
    light.color = lightData.color;
    light.intensity = lightData.intensity;
    light.range = lightData.range;
    light.spotInnerAngleDegrees = lightData.spotInnerAngle;
    light.spotOuterAngleDegrees = lightData.spotOuterAngle;
    light.areaSize = lightData.areaSize;
    m_registry.emplace<LightComponent>(entity, light);
    m_sceneOrder.push_back(entity);
    return entity;
}

glm::mat4 EditorScene::BuildTransformMatrix(const TransformComponent& transform)
{
    glm::mat4 matrix(1.0f);
    matrix = glm::translate(matrix, transform.translation);
    matrix = glm::rotate(matrix, glm::radians(transform.rotationDegrees.x), glm::vec3(1.0f, 0.0f, 0.0f));
    matrix = glm::rotate(matrix, glm::radians(transform.rotationDegrees.y), glm::vec3(0.0f, 1.0f, 0.0f));
    matrix = glm::rotate(matrix, glm::radians(transform.rotationDegrees.z), glm::vec3(0.0f, 0.0f, 1.0f));
    matrix = glm::scale(matrix, glm::max(transform.scale, WorldUnits::kMinimumScale3));
    return matrix;
}

SerializedSceneData EditorScene::CaptureSceneData() const
{
    SerializedSceneData sceneData{};
    sceneData.gizmo = m_gizmoSettings;

    // The on_destroy listener keeps scene order free of stale handles,
    // so entries can be read without per-entity validity checks.
    for (entt::entity entity : m_sceneOrder)
    {
        if (!m_registry.all_of<ModelComponent>(entity))
        {
            continue;
        }
        const auto [tag, transform, model] =
            m_registry.get<TagComponent, TransformComponent, ModelComponent>(entity);

        SerializedEntityData entityData{};
        entityData.entityUuid = GetEntityUuid(entity);
        entityData.tagName = tag.name;
        entityData.modelDisplayName = model.displayName;
        entityData.modelSourcePath = model.sourcePath;
        entityData.modelSourceUuid = model.sourceUuid;
        entityData.modelBaseColorTextureOverridePath = model.baseColorTextureOverridePath;
        entityData.modelBaseColorTextureOverrideUuid = model.baseColorTextureOverrideUuid;
        entityData.transform = transform;
        sceneData.entities.push_back(entityData);
    }

    for (entt::entity entity : m_sceneOrder)
    {
        if (!m_registry.all_of<LightComponent>(entity))
        {
            continue;
        }
        const auto [tag, transform, light] =
            m_registry.get<TagComponent, TransformComponent, LightComponent>(entity);

        SerializedLightData lightData{};
        lightData.entityUuid = GetEntityUuid(entity);
        lightData.tagName = tag.name;
        lightData.lightType = light.type;
        lightData.color = light.color;
        lightData.intensity = light.intensity;
        lightData.range = light.range;
        lightData.spotInnerAngle = light.spotInnerAngleDegrees;
        lightData.spotOuterAngle = light.spotOuterAngleDegrees;
        lightData.areaSize = light.areaSize;
        lightData.transform = transform;
        sceneData.lights.push_back(lightData);
    }

    if (HasSelection())
    {
        sceneData.selectedEntityUuid = GetEntityUuid(m_selectedEntity);
        size_t modelIndex = 0;
        for (entt::entity entity : m_sceneOrder)
        {
            if (!m_registry.all_of<ModelComponent>(entity))
            {
                continue;
            }
            if (entity == m_selectedEntity)
            {
                sceneData.selectedEntityIndex = static_cast<int>(modelIndex);
                break;
            }
            ++modelIndex;
        }
    }

    return sceneData;
}
