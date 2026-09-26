#include "renderer_world.h"

#include <algorithm>
#include <iterator>

namespace me
{

void RendererWorld::SetSceneWorld(ISceneWorld& sceneWorld)
{
    m_sceneWorld = &sceneWorld;
}

bool RendererWorld::HasSceneWorld() const
{
    return m_sceneWorld != nullptr;
}

ISceneWorld& RendererWorld::GetSceneWorld()
{
    if (m_sceneWorld == nullptr)
    {
        throw std::runtime_error("RendererWorld has no bound scene world");
    }

    return *m_sceneWorld;
}

const ISceneWorld& RendererWorld::GetSceneWorld() const
{
    if (m_sceneWorld == nullptr)
    {
        throw std::runtime_error("RendererWorld has no bound scene world");
    }

    return *m_sceneWorld;
}

void RendererWorld::SetRenderSubmeshes(std::vector<CpuRenderSubmesh> renderSubmeshes)
{
    m_renderSubmeshes = std::move(renderSubmeshes);
}

void RendererWorld::ReplaceEntityRenderSubmeshes(
    entt::entity entity,
    std::vector<CpuRenderSubmesh> renderSubmeshes)
{
    const auto first = std::find_if(
        m_renderSubmeshes.begin(),
        m_renderSubmeshes.end(),
        [entity](const CpuRenderSubmesh& submesh)
        {
            return submesh.entity == entity;
        });
    const size_t insertionIndex = static_cast<size_t>(std::distance(m_renderSubmeshes.begin(), first));
    std::erase_if(
        m_renderSubmeshes,
        [entity](const CpuRenderSubmesh& submesh)
        {
            return submesh.entity == entity;
        });
    m_renderSubmeshes.insert(
        m_renderSubmeshes.begin() + static_cast<std::ptrdiff_t>(std::min(insertionIndex, m_renderSubmeshes.size())),
        std::make_move_iterator(renderSubmeshes.begin()),
        std::make_move_iterator(renderSubmeshes.end()));
}

bool RendererWorld::RemoveEntityRenderSubmeshes(entt::entity entity)
{
    const size_t previousSize = m_renderSubmeshes.size();
    std::erase_if(
        m_renderSubmeshes,
        [entity](const CpuRenderSubmesh& submesh)
        {
            return submesh.entity == entity;
        });
    return m_renderSubmeshes.size() != previousSize;
}

void RendererWorld::ClearRenderSubmeshes()
{
    m_renderSubmeshes.clear();
}

const std::vector<CpuRenderSubmesh>& RendererWorld::GetRenderSubmeshes() const
{
    return m_renderSubmeshes;
}

glm::mat4 RendererWorld::GetModelMatrix(entt::entity entity) const
{
    return GetSceneWorld().GetModelMatrix(entity);
}

void RendererWorld::SetModelLights(std::vector<CpuModelLight> modelLights)
{
    m_modelLights = std::move(modelLights);
}

void RendererWorld::ReplaceEntityModelLights(entt::entity entity, std::vector<CpuModelLight> modelLights)
{
    RemoveEntityModelLights(entity);
    m_modelLights.insert(m_modelLights.end(), std::make_move_iterator(modelLights.begin()), std::make_move_iterator(modelLights.end()));
}

bool RendererWorld::RemoveEntityModelLights(entt::entity entity)
{
    return std::erase_if(
               m_modelLights,
               [entity](const CpuModelLight& light)
               {
                   return light.entity == entity;
               }) != 0;
}

const std::vector<CpuModelLight>& RendererWorld::GetModelLights() const
{
    return m_modelLights;
}
}
