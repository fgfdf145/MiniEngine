#include "renderer_world.h"

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
