#pragma once

#include "renderer_world.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace me
{

// Where the entities a submesh list names are this frame. Taken from the world on the main thread
// for the render thread, which draws while the main thread changes the world.
class RenderTransformSnapshot
{
  public:
    // The model matrix of every entity the list names that the world still has, and the world's
    // submesh local transforms. Keeps its allocations from the last capture.
    void Capture(const RendererWorld& world, const CpuRenderSubmeshList& submeshes);
    void Clear();

    // Whether the capture found the entity: the list named it and the world had it.
    bool Contains(entt::entity entity) const;
    // RendererWorld::GetSubmeshModelMatrix as it was at the capture; the identity for an entity the
    // capture did not find.
    glm::mat4 GetSubmeshModelMatrix(entt::entity entity, uint32_t ordinal) const;
    size_t GetEntityCount() const
    {
        return m_entityCount;
    }

  private:
    // By entity index (entt::to_entity): the entity found there, entt::null where none, and its
    // model matrix.
    std::vector<entt::entity> m_entities;
    std::vector<glm::mat4> m_models;
    // The indices set by the last capture, cleared by the next.
    std::vector<uint32_t> m_setIndices;
    size_t m_entityCount = 0;
    std::unordered_map<entt::entity, std::vector<glm::mat4>> m_localTransforms;
};
}
