#include "editor_scene.h"

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

ImGuizmo::OPERATION ParseOperation(const std::string& value)
{
    if (value == "rotate")
    {
        return ImGuizmo::ROTATE;
    }
    if (value == "scale")
    {
        return ImGuizmo::SCALE;
    }
    return ImGuizmo::TRANSLATE;
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
    switch (operation)
    {
    case ImGuizmo::ROTATE:
        return "rotate";
    case ImGuizmo::SCALE:
        return "scale";
    case ImGuizmo::TRANSLATE:
    default:
        return "translate";
    }
}

const char* ToString(ImGuizmo::MODE mode)
{
    return mode == ImGuizmo::LOCAL ? "local" : "world";
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

    const YAML::Node gizmoNode = root["editor"]["gizmo"];
    sceneData.gizmo.operation = ParseOperation(gizmoNode["operation"].as<std::string>("translate"));
    sceneData.gizmo.mode = ParseMode(gizmoNode["mode"].as<std::string>("world"));
    sceneData.gizmo.useSnap = gizmoNode["use_snap"].as<bool>(sceneData.gizmo.useSnap);
    sceneData.gizmo.translationSnap = ReadVec3(gizmoNode["translation_snap"], sceneData.gizmo.translationSnap);
    sceneData.gizmo.rotationSnap = gizmoNode["rotation_snap"].as<float>(sceneData.gizmo.rotationSnap);
    sceneData.gizmo.scaleSnap = ReadVec3(gizmoNode["scale_snap"], sceneData.gizmo.scaleSnap);

    const YAML::Node entitiesNode = root["entities"];
    if (entitiesNode && entitiesNode.IsSequence())
    {
        for (const YAML::Node& entityNode : entitiesNode)
        {
            SerializedEntityData entityData{};
            entityData.tagName = entityNode["tag"].as<std::string>(entityData.tagName);

            const YAML::Node modelNode = entityNode["model"];
            entityData.modelDisplayName = modelNode["display_name"].as<std::string>(entityData.modelDisplayName);
            entityData.modelSourcePath = modelNode["source_path"].as<std::string>(entityData.modelSourcePath);
            entityData.modelBaseColorTextureOverridePath =
                modelNode["base_color_texture_override"].as<std::string>(entityData.modelBaseColorTextureOverridePath);

            const YAML::Node transformNode = entityNode["transform"];
            entityData.transform.translation = ReadVec3(transformNode["translation"], entityData.transform.translation);
            entityData.transform.rotationDegrees = ReadVec3(transformNode["rotation"], entityData.transform.rotationDegrees);
            entityData.transform.scale = glm::max(ReadVec3(transformNode["scale"], entityData.transform.scale), WorldUnits::kMinimumScale3);
            sceneData.entities.push_back(entityData);
        }
    }

    sceneData.selectedEntityIndex = root["scene"]["selected_entity"].as<int>(0);
    return sceneData;
}

std::string EmitSceneYaml(const SerializedSceneData& sceneData)
{
    YAML::Emitter emitter;
    emitter << YAML::BeginMap;
    emitter << YAML::Key << "scene" << YAML::Value << YAML::BeginMap;
    emitter << YAML::Key << "version" << YAML::Value << 2;
    emitter << YAML::Key << "selected_entity" << YAML::Value << sceneData.selectedEntityIndex;
    emitter << YAML::EndMap;

    emitter << YAML::Key << "entities" << YAML::Value << YAML::BeginSeq;
    for (const SerializedEntityData& entity : sceneData.entities)
    {
        emitter << YAML::BeginMap;
        emitter << YAML::Key << "tag" << YAML::Value << entity.tagName;
        emitter << YAML::Key << "model" << YAML::Value << YAML::BeginMap;
        emitter << YAML::Key << "display_name" << YAML::Value << entity.modelDisplayName;
        emitter << YAML::Key << "source_path" << YAML::Value << entity.modelSourcePath;
        emitter << YAML::Key << "base_color_texture_override" << YAML::Value << entity.modelBaseColorTextureOverridePath;
        emitter << YAML::EndMap;
        emitter << YAML::Key << "transform" << YAML::Value << YAML::BeginMap;
        emitter << YAML::Key << "translation" << YAML::Value << YAML::Flow << YAML::BeginSeq
                << entity.transform.translation.x << entity.transform.translation.y << entity.transform.translation.z << YAML::EndSeq;
        emitter << YAML::Key << "rotation" << YAML::Value << YAML::Flow << YAML::BeginSeq
                << entity.transform.rotationDegrees.x << entity.transform.rotationDegrees.y << entity.transform.rotationDegrees.z << YAML::EndSeq;
        emitter << YAML::Key << "scale" << YAML::Value << YAML::Flow << YAML::BeginSeq
                << entity.transform.scale.x << entity.transform.scale.y << entity.transform.scale.z << YAML::EndSeq;
        emitter << YAML::EndMap;
        emitter << YAML::EndMap;
    }
    emitter << YAML::EndSeq;

    emitter << YAML::Key << "editor" << YAML::Value << YAML::BeginMap;
    emitter << YAML::Key << "gizmo" << YAML::Value << YAML::BeginMap;
    emitter << YAML::Key << "operation" << YAML::Value << ToString(sceneData.gizmo.operation);
    emitter << YAML::Key << "mode" << YAML::Value << ToString(sceneData.gizmo.mode);
    emitter << YAML::Key << "use_snap" << YAML::Value << sceneData.gizmo.useSnap;
    emitter << YAML::Key << "translation_snap" << YAML::Value << YAML::Flow << YAML::BeginSeq
            << sceneData.gizmo.translationSnap.x << sceneData.gizmo.translationSnap.y << sceneData.gizmo.translationSnap.z << YAML::EndSeq;
    emitter << YAML::Key << "rotation_snap" << YAML::Value << sceneData.gizmo.rotationSnap;
    emitter << YAML::Key << "scale_snap" << YAML::Value << YAML::Flow << YAML::BeginSeq
            << sceneData.gizmo.scaleSnap.x << sceneData.gizmo.scaleSnap.y << sceneData.gizmo.scaleSnap.z << YAML::EndSeq;
    emitter << YAML::EndMap;
    emitter << YAML::EndMap;
    emitter << YAML::EndMap;

    return emitter.c_str();
}
}

EditorScene::EditorScene() = default;

void EditorScene::LoadConfig(const std::string& path)
{
    m_configPath = path;

    if (!std::filesystem::exists(path))
    {
        return;
    }

    const YAML::Node root = YAML::LoadFile(path);
    const YAML::Node transformNode = root["entity"]["transform"];
    m_defaultTransform.translation = ReadVec3(transformNode["translation"], m_defaultTransform.translation);
    m_defaultTransform.rotationDegrees = ReadVec3(transformNode["rotation"], m_defaultTransform.rotationDegrees);
    m_defaultTransform.scale = glm::max(ReadVec3(transformNode["scale"], m_defaultTransform.scale), WorldUnits::kMinimumScale3);

    const YAML::Node gizmoNode = root["editor"]["gizmo"];
    m_gizmoSettings.operation = ParseOperation(gizmoNode["operation"].as<std::string>("translate"));
    m_gizmoSettings.mode = ParseMode(gizmoNode["mode"].as<std::string>("world"));
    m_gizmoSettings.useSnap = gizmoNode["use_snap"].as<bool>(m_gizmoSettings.useSnap);
    m_gizmoSettings.translationSnap = ReadVec3(gizmoNode["translation_snap"], m_gizmoSettings.translationSnap);
    m_gizmoSettings.rotationSnap = gizmoNode["rotation_snap"].as<float>(m_gizmoSettings.rotationSnap);
    m_gizmoSettings.scaleSnap = ReadVec3(gizmoNode["scale_snap"], m_gizmoSettings.scaleSnap);
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
    m_registry.clear();
    m_entityOrder.clear();
    m_selectedEntity = entt::null;
}

entt::entity EditorScene::CreateEntity(const SerializedEntityData& entityData)
{
    entt::entity entity = m_registry.create();
    m_registry.emplace<TagComponent>(entity, TagComponent{ entityData.tagName });
    m_registry.emplace<TransformComponent>(entity, entityData.transform);
    m_registry.emplace<ModelComponent>(entity, ModelComponent{
        entityData.modelSourcePath,
        entityData.modelDisplayName,
        entityData.modelBaseColorTextureOverridePath,
        1,
        WorldUnits::kDefaultCubeMinBoundsMeters,
        WorldUnits::kDefaultCubeMaxBoundsMeters,
        true,
        {},
        {}
    });
    m_entityOrder.push_back(entity);
    if (m_selectedEntity == entt::null)
    {
        m_selectedEntity = entity;
    }
    return entity;
}

bool EditorScene::HasEntities() const
{
    return !m_entityOrder.empty();
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

const std::vector<entt::entity>& EditorScene::GetEntityOrder() const
{
    return m_entityOrder;
}

TagComponent& EditorScene::GetTag(entt::entity entity)
{
    return m_registry.get<TagComponent>(entity);
}

const TagComponent& EditorScene::GetTag(entt::entity entity) const
{
    return m_registry.get<TagComponent>(entity);
}

TransformComponent& EditorScene::GetTransform(entt::entity entity)
{
    return m_registry.get<TransformComponent>(entity);
}

const TransformComponent& EditorScene::GetTransform(entt::entity entity) const
{
    return m_registry.get<TransformComponent>(entity);
}

ModelComponent& EditorScene::GetModel(entt::entity entity)
{
    return m_registry.get<ModelComponent>(entity);
}

const ModelComponent& EditorScene::GetModel(entt::entity entity) const
{
    return m_registry.get<ModelComponent>(entity);
}

TagComponent& EditorScene::GetSelectedTag()
{
    EnsureSelection();
    return GetTag(m_selectedEntity);
}

const TagComponent& EditorScene::GetSelectedTag() const
{
    return m_registry.get<TagComponent>(m_selectedEntity);
}

TransformComponent& EditorScene::GetSelectedTransform()
{
    EnsureSelection();
    return GetTransform(m_selectedEntity);
}

const TransformComponent& EditorScene::GetSelectedTransform() const
{
    return m_registry.get<TransformComponent>(m_selectedEntity);
}

ModelComponent& EditorScene::GetSelectedModel()
{
    EnsureSelection();
    return GetModel(m_selectedEntity);
}

const ModelComponent& EditorScene::GetSelectedModel() const
{
    return m_registry.get<ModelComponent>(m_selectedEntity);
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

    GetSelectedTransform() = m_defaultTransform;
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
    ModelComponent& model = GetModel(entity);
    model.displayName = SanitizeName(displayName, model.displayName);
    model.sourcePath = sourcePath;
    model.submeshCount = std::max(submeshCount, 1u);
    model.minBounds = minBounds;
    model.maxBounds = maxBounds;
    model.hasBounds = hasBounds;
    model.importedMaterials = importedMaterials;
    model.importedSubmeshes = importedSubmeshes;

    if (sourcePath.empty())
    {
        GetTag(entity).name = model.displayName;
    }
    else
    {
        const std::filesystem::path modelPath(sourcePath);
        GetTag(entity).name = SanitizeName(modelPath.stem().string(), model.displayName);
    }
}

glm::mat4 EditorScene::GetModelMatrix(entt::entity entity) const
{
    return BuildTransformMatrix(GetTransform(entity));
}

void EditorScene::ApplyTransformMatrix(entt::entity entity, const glm::mat4& matrix)
{
    TransformComponent& transform = GetTransform(entity);
    transform = DecomposeTransformMatrix(matrix, transform);
}

glm::vec3 EditorScene::GetBoundsCenter(entt::entity entity) const
{
    const ModelComponent& model = GetModel(entity);
    return (model.minBounds + model.maxBounds) * 0.5f;
}

void EditorScene::ForEachEntity(const SceneEntityVisitor& visitor) const
{
    for (entt::entity entity : m_entityOrder)
    {
        if (!IsValidEntity(entity))
        {
            continue;
        }

        visitor(entity, GetTag(entity), GetTransform(entity), GetModel(entity));
    }
}

void EditorScene::ApplySceneData(const SerializedSceneData& sceneData)
{
    Clear();
    m_gizmoSettings = sceneData.gizmo;

    for (const SerializedEntityData& entityData : sceneData.entities)
    {
        CreateEntity(entityData);
    }

    if (!m_entityOrder.empty())
    {
        const int clampedIndex = std::clamp(sceneData.selectedEntityIndex, 0, static_cast<int>(m_entityOrder.size()) - 1);
        m_selectedEntity = m_entityOrder[static_cast<size_t>(clampedIndex)];
    }
}

void EditorScene::SaveSceneToFile(const std::string& path) const
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open())
    {
        throw std::runtime_error("Failed to open scene file for writing: " + path);
    }

    output << EmitSceneYaml(CaptureSceneData());
    if (!output.good())
    {
        throw std::runtime_error("Failed to write scene file: " + path);
    }
}

SerializedSceneData EditorScene::LoadSceneDataFromFile(const std::string& path)
{
    return ReadSceneData(YAML::LoadFile(path));
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

bool EditorScene::IsValidEntity(entt::entity entity) const
{
    return entity != entt::null && m_registry.valid(entity);
}

void EditorScene::EnsureSelection()
{
    if (HasSelection())
    {
        return;
    }

    if (!m_entityOrder.empty())
    {
        m_selectedEntity = m_entityOrder.front();
    }
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

    for (entt::entity entity : m_entityOrder)
    {
        if (!IsValidEntity(entity))
        {
            continue;
        }

        SerializedEntityData entityData{};
        entityData.tagName = GetTag(entity).name;
        entityData.modelDisplayName = GetModel(entity).displayName;
        entityData.modelSourcePath = GetModel(entity).sourcePath;
        entityData.modelBaseColorTextureOverridePath = GetModel(entity).baseColorTextureOverridePath;
        entityData.transform = GetTransform(entity);
        sceneData.entities.push_back(entityData);
    }

    if (HasSelection())
    {
        for (size_t index = 0; index < m_entityOrder.size(); ++index)
        {
            if (m_entityOrder[index] == m_selectedEntity)
            {
                sceneData.selectedEntityIndex = static_cast<int>(index);
                break;
            }
        }
    }

    return sceneData;
}
