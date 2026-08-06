#pragma once

#include "scene_components.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <vector>

class ISceneWorld
{
  public:
    virtual ~ISceneWorld() = default;

    // Read-only ECS access supports efficient views without letting callers
    // create/destroy entities or add/remove components behind scene invariants.
    virtual const entt::registry& Registry() const = 0;

    // Stable editor/serialization order. Runtime systems should prefer views.
    virtual const std::vector<entt::entity>& GetSceneOrder() const = 0;
    virtual bool IsValidEntity(entt::entity entity) const = 0;
    virtual TagComponent& EditTag(entt::entity entity) = 0;
    virtual TransformComponent& EditTransform(entt::entity entity) = 0;
    // Model edits become render-visible only after MarkModelRenderableDirty.
    virtual ModelComponent& EditModel(entt::entity entity) = 0;
    virtual void MarkTransformDirty(entt::entity entity) = 0;
    virtual void MarkModelRenderableDirty(entt::entity entity) = 0;
    virtual void ClearModelRenderableDirty(entt::entity entity) = 0;
    virtual void ClearAllModelRenderableDirty() = 0;
    virtual std::vector<entt::entity> GetDirtyModelRenderableEntities() const = 0;
    virtual void FlushDirtyTransforms() = 0;
    virtual glm::mat4 GetModelMatrix(entt::entity entity) const = 0;
    virtual void ApplyTransformMatrix(entt::entity entity, const glm::mat4& matrix) = 0;
    virtual glm::vec3 GetBoundsCenter(entt::entity entity) const = 0;

    const TagComponent& GetTag(entt::entity entity) const
    {
        return Registry().get<TagComponent>(entity);
    }
    const TransformComponent& GetTransform(entt::entity entity) const
    {
        return Registry().get<TransformComponent>(entity);
    }
    const ModelComponent& GetModel(entt::entity entity) const
    {
        return Registry().get<ModelComponent>(entity);
    }
    const ModelBoundsComponent& GetModelBounds(entt::entity entity) const
    {
        return Registry().get<ModelBoundsComponent>(entity);
    }
    const EditorModelMetadataComponent& GetModelMetadata(entt::entity entity) const
    {
        return Registry().get<EditorModelMetadataComponent>(entity);
    }
    const std::string& GetEntityUuid(entt::entity entity) const
    {
        return Registry().get<SceneEntityIdComponent>(entity).value;
    }

    bool HasModelComponent(entt::entity entity) const
    {
        return IsValidEntity(entity) && Registry().all_of<ModelComponent>(entity);
    }
};
