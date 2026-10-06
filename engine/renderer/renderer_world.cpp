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

std::vector<std::shared_ptr<const CpuRenderSubmesh>> RendererWorld::Number(std::vector<CpuRenderSubmesh> renderSubmeshes)
{
    std::vector<std::shared_ptr<const CpuRenderSubmesh>> numbered;
    numbered.reserve(renderSubmeshes.size());
    for (CpuRenderSubmesh& submesh : renderSubmeshes)
    {
        submesh.revision = m_nextRevision++;
        numbered.push_back(std::make_shared<const CpuRenderSubmesh>(std::move(submesh)));
    }
    return numbered;
}

void RendererWorld::SetRenderSubmeshes(std::vector<CpuRenderSubmesh> renderSubmeshes)
{
    m_renderSubmeshes = Number(std::move(renderSubmeshes));
    m_snapshot.reset();
}

void RendererWorld::ReplaceEntityRenderSubmeshes(
    entt::entity entity,
    std::vector<CpuRenderSubmesh> renderSubmeshes)
{
    const auto first = std::find_if(
        m_renderSubmeshes.begin(),
        m_renderSubmeshes.end(),
        [entity](const std::shared_ptr<const CpuRenderSubmesh>& submesh)
        {
            return submesh->entity == entity;
        });
    const size_t insertionIndex = static_cast<size_t>(std::distance(m_renderSubmeshes.begin(), first));
    std::vector<std::shared_ptr<const CpuRenderSubmesh>> numbered = Number(std::move(renderSubmeshes));
    std::erase_if(
        m_renderSubmeshes,
        [entity](const std::shared_ptr<const CpuRenderSubmesh>& submesh)
        {
            return submesh->entity == entity;
        });
    m_renderSubmeshes.insert(
        m_renderSubmeshes.begin() + static_cast<std::ptrdiff_t>(std::min(insertionIndex, m_renderSubmeshes.size())),
        std::make_move_iterator(numbered.begin()),
        std::make_move_iterator(numbered.end()));
    m_snapshot.reset();
}

bool RendererWorld::RemoveEntityRenderSubmeshes(entt::entity entity)
{
    const size_t previousSize = m_renderSubmeshes.size();
    std::erase_if(
        m_renderSubmeshes,
        [entity](const std::shared_ptr<const CpuRenderSubmesh>& submesh)
        {
            return submesh->entity == entity;
        });
    if (m_renderSubmeshes.size() == previousSize)
    {
        return false;
    }
    m_snapshot.reset();
    return true;
}

void RendererWorld::ClearRenderSubmeshes()
{
    m_renderSubmeshes.clear();
    m_snapshot.reset();
}

const CpuRenderSubmeshList& RendererWorld::GetRenderSubmeshes() const
{
    return m_renderSubmeshes;
}

std::shared_ptr<const CpuRenderSubmeshList> RendererWorld::SnapshotRenderSubmeshes() const
{
    if (!m_snapshot)
    {
        m_snapshot = std::make_shared<const CpuRenderSubmeshList>(m_renderSubmeshes);
    }
    return m_snapshot;
}

glm::mat4 RendererWorld::GetModelMatrix(entt::entity entity) const
{
    return GetSceneWorld().GetModelMatrix(entity);
}

void RendererWorld::SetSubmeshLocalTransforms(entt::entity entity, std::vector<glm::mat4> transforms)
{
    m_submeshLocalTransforms[entity] = std::move(transforms);
}

void RendererWorld::ClearSubmeshLocalTransforms(entt::entity entity)
{
    m_submeshLocalTransforms.erase(entity);
}

glm::mat4 RendererWorld::GetSubmeshModelMatrix(entt::entity entity, uint32_t ordinal) const
{
    const glm::mat4 model = GetModelMatrix(entity);
    const auto found = m_submeshLocalTransforms.find(entity);
    if (found == m_submeshLocalTransforms.end() || ordinal >= found->second.size())
    {
        return model;
    }
    return model * found->second[ordinal];
}

const std::unordered_map<entt::entity, std::vector<glm::mat4>>& RendererWorld::GetSubmeshLocalTransforms() const
{
    return m_submeshLocalTransforms;
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
