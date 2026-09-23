#include "world_bounds.h"

#include <limits>

namespace me
{

std::array<glm::vec3, 8> BuildBoundsCorners(const glm::vec3& minBounds, const glm::vec3& maxBounds)
{
    return {
        glm::vec3(minBounds.x, minBounds.y, minBounds.z),
        glm::vec3(maxBounds.x, minBounds.y, minBounds.z),
        glm::vec3(minBounds.x, maxBounds.y, minBounds.z),
        glm::vec3(maxBounds.x, maxBounds.y, minBounds.z),
        glm::vec3(minBounds.x, minBounds.y, maxBounds.z),
        glm::vec3(maxBounds.x, minBounds.y, maxBounds.z),
        glm::vec3(minBounds.x, maxBounds.y, maxBounds.z),
        glm::vec3(maxBounds.x, maxBounds.y, maxBounds.z)};
}

bool ComputeWorldModelBounds(const IEditorWorld& world, entt::entity entity, glm::vec3& minBounds, glm::vec3& maxBounds)
{
    const ModelBoundsComponent& bounds = world.GetModelBounds(entity);
    if (!bounds.hasBounds)
    {
        return false;
    }

    glm::vec3 worldMin(std::numeric_limits<float>::max());
    glm::vec3 worldMax(std::numeric_limits<float>::lowest());
    const glm::mat4 modelMatrix = world.GetModelMatrix(entity);
    for (const glm::vec3& corner : BuildBoundsCorners(bounds.minBounds, bounds.maxBounds))
    {
        const glm::vec3 worldPoint = glm::vec3(modelMatrix * glm::vec4(corner, 1.0f));
        worldMin = glm::min(worldMin, worldPoint);
        worldMax = glm::max(worldMax, worldPoint);
    }
    minBounds = worldMin;
    maxBounds = worldMax;
    return true;
}
}
