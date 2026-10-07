#include "render_transform_snapshot.h"

namespace me
{

void RenderTransformSnapshot::Capture(const RendererWorld& world, const CpuRenderSubmeshList& submeshes)
{
    Clear();
    const ISceneWorld& sceneWorld = world.GetSceneWorld();
    for (const std::shared_ptr<const CpuRenderSubmesh>& submesh : submeshes)
    {
        const entt::entity entity = submesh->entity;
        const uint32_t index = static_cast<uint32_t>(entt::to_entity(entity));
        if (index < m_entities.size() && m_entities[index] == entity)
        {
            continue;
        }
        // An entity deleted this frame stays in the list until the renderables refresh; it is not
        // drawn any more.
        if (!sceneWorld.IsValidEntity(entity))
        {
            continue;
        }
        if (index >= m_entities.size())
        {
            m_entities.resize(index + 1, entt::null);
            m_models.resize(index + 1);
        }
        m_entities[index] = entity;
        m_models[index] = world.GetModelMatrix(entity);
        m_setIndices.push_back(index);
    }
    m_entityCount = m_setIndices.size();
    for (const auto& [entity, transforms] : world.GetSubmeshLocalTransforms())
    {
        m_localTransforms[entity] = transforms;
    }
    for (const auto& [entity, palette] : world.GetJointPalettes())
    {
        m_jointPalettes[entity] = palette;
    }
}

void RenderTransformSnapshot::Clear()
{
    for (const uint32_t index : m_setIndices)
    {
        m_entities[index] = entt::null;
    }
    m_setIndices.clear();
    m_entityCount = 0;
    m_localTransforms.clear();
    m_jointPalettes.clear();
}

const std::vector<glm::mat4>* RenderTransformSnapshot::GetJointPalette(entt::entity entity) const
{
    const auto found = m_jointPalettes.find(entity);
    return found == m_jointPalettes.end() ? nullptr : &found->second;
}

bool RenderTransformSnapshot::Contains(entt::entity entity) const
{
    const uint32_t index = static_cast<uint32_t>(entt::to_entity(entity));
    return entity != entt::null && index < m_entities.size() && m_entities[index] == entity;
}

glm::mat4 RenderTransformSnapshot::GetSubmeshModelMatrix(entt::entity entity, uint32_t ordinal) const
{
    if (!Contains(entity))
    {
        return glm::mat4(1.0f);
    }
    const glm::mat4& model = m_models[static_cast<uint32_t>(entt::to_entity(entity))];
    const auto found = m_localTransforms.find(entity);
    if (found == m_localTransforms.end() || ordinal >= found->second.size())
    {
        return model;
    }
    return model * found->second[ordinal];
}
}
