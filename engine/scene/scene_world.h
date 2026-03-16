#pragma once

#include "scene_components.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <functional>
#include <vector>

using SceneEntityVisitor = std::function<void(entt::entity, const TagComponent&, const TransformComponent&, const ModelComponent&)>;

class ISceneWorld
{
public:
    virtual ~ISceneWorld() = default;

    virtual const std::vector<entt::entity>& GetEntityOrder() const = 0;
    virtual TagComponent& GetTag(entt::entity entity) = 0;
    virtual const TagComponent& GetTag(entt::entity entity) const = 0;
    virtual TransformComponent& GetTransform(entt::entity entity) = 0;
    virtual const TransformComponent& GetTransform(entt::entity entity) const = 0;
    virtual ModelComponent& GetModel(entt::entity entity) = 0;
    virtual const ModelComponent& GetModel(entt::entity entity) const = 0;
    virtual glm::mat4 GetModelMatrix(entt::entity entity) const = 0;
    virtual void ApplyTransformMatrix(entt::entity entity, const glm::mat4& matrix) = 0;
    virtual glm::vec3 GetBoundsCenter(entt::entity entity) const = 0;
    virtual void ForEachEntity(const SceneEntityVisitor& visitor) const = 0;
};
