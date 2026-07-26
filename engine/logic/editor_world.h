#pragma once

#include <logic_layer.h>
#include <gizmo_settings.h>
#include <scene_components.h>
#include <scene_world.h>

#include <imgui.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct SerializedEntityData
{
    std::string entityUuid;
    std::string tagName = "Cube";
    std::string modelDisplayName = "Cube";
    std::string modelSourcePath;
    std::string modelSourceUuid;   // stable asset id; survives renames/moves of the path
    std::string modelBaseColorTextureOverridePath;
    std::string modelBaseColorTextureOverrideUuid;
    TransformComponent transform;
};

struct SerializedLightData
{
    std::string entityUuid;
    std::string tagName = "Light";
    LightType lightType = LightType::Point;
    glm::vec3 color{ 1.0f, 1.0f, 1.0f };
    float intensity = 1000.0f;
    float range = 10.0f;
    float spotInnerAngle = 15.0f;
    float spotOuterAngle = 30.0f;
    glm::vec2 areaSize{ 1.0f, 1.0f };
    TransformComponent transform;
};

struct SerializedSceneData
{
    std::vector<SerializedEntityData> entities;
    std::vector<SerializedLightData> lights;
    GizmoSettings gizmo;
    std::string selectedEntityUuid;
    // Legacy v1/v2 model-list index, retained for backward-compatible loads.
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
    virtual entt::entity CreateLightEntity(const SerializedLightData& lightData) = 0;
    // Destroys any scene entity (model or light); scene order and selection
    // are kept in sync by the implementation regardless of entity kind.
    virtual void DestroyEntity(entt::entity entity) = 0;

    virtual bool HasEntities() const = 0;
    virtual bool HasSelection() const = 0;
    virtual bool IsSelected(entt::entity entity) const = 0;
    virtual entt::entity GetSelectedEntity() const = 0;
    virtual void SetSelectedEntity(entt::entity entity) = 0;
    virtual void ClearSelection() = 0;

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
    virtual SerializedSceneData CaptureSceneData() const = 0;
    virtual void SaveSceneToFile(const std::string& path) const = 0;
    virtual std::string BuildSceneYamlPreview() const = 0;
    virtual const std::string& GetConfigPath() const = 0;
    virtual const std::string& GetSceneFilePath() const = 0;

    // Convenience accessors; callers must hold a valid selection (HasSelection()).
    TagComponent& EditSelectedTag() { return EditTag(GetSelectedEntity()); }
    const TagComponent& GetSelectedTag() const { return GetTag(GetSelectedEntity()); }
    TransformComponent& EditSelectedTransform() { return EditTransform(GetSelectedEntity()); }
    const TransformComponent& GetSelectedTransform() const { return GetTransform(GetSelectedEntity()); }
    ModelComponent& EditSelectedModel() { return EditModel(GetSelectedEntity()); }
    const ModelComponent& GetSelectedModel() const { return GetModel(GetSelectedEntity()); }
    const ModelBoundsComponent& GetSelectedModelBounds() const { return GetModelBounds(GetSelectedEntity()); }
    const EditorModelMetadataComponent& GetSelectedModelMetadata() const { return GetModelMetadata(GetSelectedEntity()); }

    bool HasLightComponent(entt::entity entity) const
    {
        return IsValidEntity(entity) && Registry().all_of<LightComponent>(entity);
    }
    virtual LightComponent& EditLightComponent(entt::entity entity) = 0;
    const LightComponent& GetLightComponent(entt::entity entity) const { return Registry().get<LightComponent>(entity); }

    template<typename Visitor>
    void ForEachLight(Visitor&& visitor) const
    {
        const entt::registry& registry = Registry();
        registry.view<const TagComponent, const TransformComponent, const LightComponent>()
            .each(std::forward<Visitor>(visitor));
    }
};

std::unique_ptr<IEditorWorld> CreateEditorWorld();
SerializedSceneData LoadEditorSceneDataFromFile(const std::string& path);
void SaveEditorSceneDataToFile(const SerializedSceneData& sceneData, const std::string& path);
