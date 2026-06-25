#pragma once

#include "editor_world.h"

class EditorScene final : public IEditorWorld
{
public:
    EditorScene();

    void LoadConfig(const std::string& path);
    void SetSceneFilePath(const std::string& path);
    void CreateTwoCubeTestScene();
    void Clear();
    entt::entity CreateEntity(const SerializedEntityData& entityData);
    void DestroyEntity(entt::entity entity);

    bool HasEntities() const;
    bool HasSelection() const;
    bool IsSelected(entt::entity entity) const;
    entt::entity GetSelectedEntity() const;
    void SetSelectedEntity(entt::entity entity);
    void ClearSelection();
    const std::vector<entt::entity>& GetEntityOrder() const override;

    TagComponent& GetTag(entt::entity entity) override;
    const TagComponent& GetTag(entt::entity entity) const override;
    TransformComponent& GetTransform(entt::entity entity) override;
    const TransformComponent& GetTransform(entt::entity entity) const override;
    ModelComponent& GetModel(entt::entity entity) override;
    const ModelComponent& GetModel(entt::entity entity) const override;

    TagComponent& GetSelectedTag();
    const TagComponent& GetSelectedTag() const;
    TransformComponent& GetSelectedTransform();
    const TransformComponent& GetSelectedTransform() const;
    ModelComponent& GetSelectedModel();
    const ModelComponent& GetSelectedModel() const;

    GizmoSettings& GetGizmoSettings();
    const GizmoSettings& GetGizmoSettings() const;

    void ResetSelectedTransform();
    void UpdateModelInfo(
        entt::entity entity,
        const std::string& displayName,
        const std::string& sourcePath,
        uint32_t submeshCount,
        const glm::vec3& minBounds,
        const glm::vec3& maxBounds,
        bool hasBounds,
        const std::vector<ModelImportedMaterialInfo>& importedMaterials,
        const std::vector<ModelImportedSubmeshInfo>& importedSubmeshes
    );

    glm::mat4 GetModelMatrix(entt::entity entity) const override;
    void ApplyTransformMatrix(entt::entity entity, const glm::mat4& matrix) override;
    glm::vec3 GetBoundsCenter(entt::entity entity) const override;
    void ForEachEntity(const SceneEntityVisitor& visitor) const override;

    void ApplySceneData(const SerializedSceneData& sceneData);
    void SaveSceneToFile(const std::string& path) const;
    std::string BuildSceneYamlPreview() const;
    const std::string& GetConfigPath() const;
    const std::string& GetSceneFilePath() const;

    // Light entity management
    entt::entity CreateLightEntity(const SerializedLightData& lightData) override;
    void DestroyLightEntity(entt::entity entity) override;
    bool HasLightComponent(entt::entity entity) const override;
    LightComponent& GetLightComponent(entt::entity entity) override;
    const LightComponent& GetLightComponent(entt::entity entity) const override;
    const std::vector<entt::entity>& GetLightOrder() const override;
    void ForEachLight(
        const std::function<void(entt::entity, const TagComponent&, const TransformComponent&, const LightComponent&)>& visitor
    ) const override;

private:
    bool IsValidEntity(entt::entity entity) const;
    void EnsureSelection();
    static glm::mat4 BuildTransformMatrix(const TransformComponent& transform);
    SerializedSceneData CaptureSceneData() const;

    entt::registry m_registry;
    std::vector<entt::entity> m_entityOrder;
    std::vector<entt::entity> m_lightOrder;
    entt::entity m_selectedEntity = entt::null;
    TransformComponent m_defaultTransform;
    GizmoSettings m_gizmoSettings;
    std::string m_configPath;
    std::string m_sceneFilePath;
};
