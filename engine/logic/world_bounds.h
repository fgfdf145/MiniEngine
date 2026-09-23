#pragma once

#include <engine/logic/editor_world.h>

#include <glm/glm.hpp>

#include <array>

namespace me
{

// The eight corners of an axis-aligned box.
std::array<glm::vec3, 8> BuildBoundsCorners(const glm::vec3& minBounds, const glm::vec3& maxBounds);

// The entity's model bounds in world space: the axis-aligned box around its eight local corners
// transformed by the model matrix. ModelBoundsComponent is local to the model, so anything placing
// the camera or testing against the scene needs this rather than the component. False, leaving the
// outputs untouched, when the entity has no bounds.
bool ComputeWorldModelBounds(const IEditorWorld& world, entt::entity entity, glm::vec3& minBounds, glm::vec3& maxBounds);
}
