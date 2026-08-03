#pragma once

#include "editor_world.h"

#include <unordered_map>

class EditorScene final : public IEditorWorld
{
  public:
    EditorScene();

    void LoadConfig(const std::string& path) override;
    void SetSceneFilePath(const std::string& path) override;
    void CreateTwoCubeTestScene() override;
    void Clear() override;
    entt::entity CreateEntity(const SerializedEntityData& entityData) override;
    entt::entity CreateLightEntity(const SerializedLightData& lightData) override;
    void DestroyEntity(entt::entity entity) override;

    bool HasEntities() const override;
    bool HasSelection() const override;
    bool IsSelected(entt::entity entity) const override;
    entt::entity GetSelectedEntity() const override;
    void SetSelectedEntity(entt::entity entity) override;
    void ClearSelection() override;

    const entt::registry& Registry() const override
    {
        return m_registry;
    }
    const std::vector<entt::entity>& GetSceneOrder() const override;
    bool IsValidEntity(entt::entity entity) const override;
    TagComponent& EditTag(entt::entity entity) override;
    TransformComponent& EditTransform(entt::entity entity) override;
    ModelComponent& EditModel(entt::entity entity) override;
    LightComponent& EditLightComponent(entt::entity entity) override;
    void MarkTransformDirty(entt::entity entity) override;
    void MarkModelRenderableDirty(entt::entity entity) override;
    void ClearModelRenderableDirty(entt::entity entity) override;
    void ClearAllModelRenderableDirty() override;
    std::vector<entt::entity> GetDirtyModelRenderableEntities() const override;
    void FlushDirtyTransforms() override;

    GizmoSettings& GetGizmoSettings() override;
    const GizmoSettings& GetGizmoSettings() const override;

    void ResetSelectedTransform() override;
    void UpdateModelInfo(
        entt::entity entity,
        const std::string& displayName,
        const std::string& sourcePath,
        uint32_t submeshCount,
        const glm::vec3& minBounds,
        const glm::vec3& maxBounds,
        bool hasBounds,
        const std::vector<ModelImportedMaterialInfo>& importedMaterials,
        const std::vector<ModelImportedSubmeshInfo>& importedSubmeshes) override;

    glm::mat4 GetModelMatrix(entt::entity entity) const override;
    void ApplyTransformMatrix(entt::entity entity, const glm::mat4& matrix) override;
    glm::vec3 GetBoundsCenter(entt::entity entity) const override;

    void ApplySceneData(const SerializedSceneData& sceneData) override;
    SerializedSceneData CaptureSceneData() const override;
    void SaveSceneToFile(const std::string& path) const override;
    std::string BuildSceneYamlPreview() const override;
    const std::string& GetConfigPath() const override;
    const std::string& GetSceneFilePath() const override;

  private:
    void EnsureSelection();
    void OnEntityDestroyed(entt::registry& registry, entt::entity entity);
    void OnSceneEntityIdDestroyed(entt::registry& registry, entt::entity entity);
    std::string AdoptOrCreateEntityUuid(const std::string& requestedUuid);
    static glm::mat4 BuildTransformMatrix(const TransformComponent& transform);

    std::vector<entt::entity> m_sceneOrder;
    entt::entity m_selectedEntity = entt::null;
    TransformComponent m_defaultTransform;
    GizmoSettings m_gizmoSettings;
    std::string m_configPath;
    std::string m_sceneFilePath;
    std::unordered_map<std::string, entt::entity> m_entityByUuid;
    // Declared last: the registry is destroyed first, so its on_destroy
    // listeners can never observe already-destroyed members above.
    entt::registry m_registry;
};
