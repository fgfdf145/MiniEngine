#include "renderer_world.h"

#include <algorithm>
#include <glm/common.hpp>
#include <iterator>

glm::vec3 ComputeMeshBoundsCenter(const MeshData& mesh)
{
    if (mesh.vertices.empty())
    {
        return glm::vec3(0.0f);
    }
    glm::vec3 minimum(mesh.vertices.front().position[0], mesh.vertices.front().position[1], mesh.vertices.front().position[2]);
    glm::vec3 maximum = minimum;
    for (const Vertex& vertex : mesh.vertices)
    {
        const glm::vec3 position(vertex.position[0], vertex.position[1], vertex.position[2]);
        minimum = glm::min(minimum, position);
        maximum = glm::max(maximum, position);
    }
    return (minimum + maximum) * 0.5f;
}

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
    std::vector<CpuRenderSubmesh> renderSubmeshes
)
{
    const auto first = std::find_if(
        m_renderSubmeshes.begin(),
        m_renderSubmeshes.end(),
        [entity](const CpuRenderSubmesh& submesh) { return submesh.entity == entity; }
    );
    const size_t insertionIndex = static_cast<size_t>(std::distance(m_renderSubmeshes.begin(), first));
    std::erase_if(
        m_renderSubmeshes,
        [entity](const CpuRenderSubmesh& submesh) { return submesh.entity == entity; }
    );
    m_renderSubmeshes.insert(
        m_renderSubmeshes.begin() + static_cast<std::ptrdiff_t>(std::min(insertionIndex, m_renderSubmeshes.size())),
        std::make_move_iterator(renderSubmeshes.begin()),
        std::make_move_iterator(renderSubmeshes.end())
    );
}

bool RendererWorld::RemoveEntityRenderSubmeshes(entt::entity entity)
{
    const size_t previousSize = m_renderSubmeshes.size();
    std::erase_if(
        m_renderSubmeshes,
        [entity](const CpuRenderSubmesh& submesh) { return submesh.entity == entity; }
    );
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
