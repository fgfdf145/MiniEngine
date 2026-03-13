#pragma once

#include <imgui.h>
#include <ImGuizmo.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct TagComponent
{
    std::string name = "Cube";
};

struct TransformComponent
{
    glm::vec3 translation{ 0.0f, 0.0f, 0.0f };
    glm::vec3 rotationDegrees{ 0.0f, 0.0f, 0.0f };
    glm::vec3 scale{ 1.0f, 1.0f, 1.0f };
};

struct ModelComponent
{
    std::string sourcePath;
    std::string displayName = "Cube";
    uint32_t submeshCount = 1;
    glm::vec3 minBounds{ -0.5f, -0.5f, -0.5f };
    glm::vec3 maxBounds{ 0.5f, 0.5f, 0.5f };
    bool hasBounds = true;
};

struct GizmoSettings
{
    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE mode = ImGuizmo::WORLD;
    bool useSnap = false;
    glm::vec3 translationSnap{ 0.5f, 0.5f, 0.5f };
    float rotationSnap = 15.0f;
    glm::vec3 scaleSnap{ 0.1f, 0.1f, 0.1f };
};

struct SerializedEntityData
{
    std::string tagName = "Cube";
    std::string modelDisplayName = "Cube";
    std::string modelSourcePath;
    TransformComponent transform;
};

struct SerializedSceneData
{
    std::vector<SerializedEntityData> entities;
    GizmoSettings gizmo;
    int selectedEntityIndex = 0;
};

class EditorScene
{
public:
    EditorScene();

    void LoadConfig(const std::string& path);
    void SetSceneFilePath(const std::string& path);
    void CreateTwoCubeTestScene();
    void Clear();
    entt::entity CreateEntity(const SerializedEntityData& entityData);

    bool HasEntities() const;
    bool HasSelection() const;
    bool IsSelected(entt::entity entity) const;
    entt::entity GetSelectedEntity() const;
    void SetSelectedEntity(entt::entity entity);
    void ClearSelection();
    const std::vector<entt::entity>& GetEntityOrder() const;

    TagComponent& GetTag(entt::entity entity);
    const TagComponent& GetTag(entt::entity entity) const;
    TransformComponent& GetTransform(entt::entity entity);
    const TransformComponent& GetTransform(entt::entity entity) const;
    ModelComponent& GetModel(entt::entity entity);
    const ModelComponent& GetModel(entt::entity entity) const;

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
        bool hasBounds
    );

    glm::mat4 GetModelMatrix(entt::entity entity) const;
    void ApplyTransformMatrix(entt::entity entity, const glm::mat4& matrix);
    glm::vec3 GetBoundsCenter(entt::entity entity) const;
    void ForEachEntity(const std::function<void(entt::entity, const TagComponent&, const TransformComponent&, const ModelComponent&)>& visitor) const;

    void ApplySceneData(const SerializedSceneData& sceneData);
    void SaveSceneToFile(const std::string& path) const;
    static SerializedSceneData LoadSceneDataFromFile(const std::string& path);
    std::string BuildSceneYamlPreview() const;
    const std::string& GetConfigPath() const;
    const std::string& GetSceneFilePath() const;

private:
    bool IsValidEntity(entt::entity entity) const;
    void EnsureSelection();
    static glm::mat4 BuildTransformMatrix(const TransformComponent& transform);
    SerializedSceneData CaptureSceneData() const;

    entt::registry m_registry;
    std::vector<entt::entity> m_entityOrder;
    entt::entity m_selectedEntity = entt::null;
    TransformComponent m_defaultTransform;
    GizmoSettings m_gizmoSettings;
    std::string m_configPath;
    std::string m_sceneFilePath;
};
