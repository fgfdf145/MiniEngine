#pragma once

#include <logic_layer.h>
#include <scene_components.h>
#include <scene_world.h>
#include <world_units.h>

#include <imgui.h>
#include <ImGuizmo.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct GizmoSettings
{
    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE mode = ImGuizmo::WORLD;
    bool useSnap = false;
    glm::vec3 translationSnap = WorldUnits::kDefaultTranslationSnapMeters;
    float rotationSnap = WorldUnits::kDefaultRotationSnapDegrees;
    glm::vec3 scaleSnap = WorldUnits::kDefaultScaleSnap;
};

struct SerializedEntityData
{
    std::string tagName = "Cube";
    std::string modelDisplayName = "Cube";
    std::string modelSourcePath;
    std::string modelBaseColorTextureOverridePath;
    TransformComponent transform;
};

struct SerializedSceneData
{
    std::vector<SerializedEntityData> entities;
    GizmoSettings gizmo;
    int selectedEntityIndex = 0;
};

class IEditorWorld : public IEditorLogicLayer, public ISceneWorld
{
public:
    ~IEditorWorld() override = default;

    virtual void LoadConfig(const std::string& path) = 0;
    virtual void SetSceneFilePath(const std::string& path) = 0;
    virtual void CreateTwoCubeTestScene() = 0;
    virtual void Clear() = 0;
    virtual entt::entity CreateEntity(const SerializedEntityData& entityData) = 0;
    virtual void DestroyEntity(entt::entity entity) = 0;

    virtual bool HasEntities() const = 0;
    virtual bool HasSelection() const = 0;
    virtual bool IsSelected(entt::entity entity) const = 0;
    virtual entt::entity GetSelectedEntity() const = 0;
    virtual void SetSelectedEntity(entt::entity entity) = 0;
    virtual void ClearSelection() = 0;

    virtual TagComponent& GetSelectedTag() = 0;
    virtual const TagComponent& GetSelectedTag() const = 0;
    virtual TransformComponent& GetSelectedTransform() = 0;
    virtual const TransformComponent& GetSelectedTransform() const = 0;
    virtual ModelComponent& GetSelectedModel() = 0;
    virtual const ModelComponent& GetSelectedModel() const = 0;

    virtual GizmoSettings& GetGizmoSettings() = 0;
    virtual const GizmoSettings& GetGizmoSettings() const = 0;

    virtual void ResetSelectedTransform() = 0;
    virtual void UpdateModelInfo(
        entt::entity entity,
        const std::string& displayName,
        const std::string& sourcePath,
        uint32_t submeshCount,
        const glm::vec3& minBounds,
        const glm::vec3& maxBounds,
        bool hasBounds,
        const std::vector<ModelImportedMaterialInfo>& importedMaterials,
        const std::vector<ModelImportedSubmeshInfo>& importedSubmeshes
    ) = 0;

    virtual void ApplySceneData(const SerializedSceneData& sceneData) = 0;
    virtual void SaveSceneToFile(const std::string& path) const = 0;
    virtual std::string BuildSceneYamlPreview() const = 0;
    virtual const std::string& GetConfigPath() const = 0;
    virtual const std::string& GetSceneFilePath() const = 0;
};

std::unique_ptr<IEditorWorld> CreateEditorWorld();
SerializedSceneData LoadEditorSceneDataFromFile(const std::string& path);
void SaveEditorSceneDataToFile(const SerializedSceneData& sceneData, const std::string& path);
