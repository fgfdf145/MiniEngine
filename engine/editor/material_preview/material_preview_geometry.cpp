#include "material_preview_geometry.h"

#include <utility>

namespace me
{

std::shared_ptr<const MaterialPreviewGeometry> MaterialPreviewGeometry::Build(std::vector<MaterialPreviewMesh> meshes)
{
    auto geometry = std::make_shared<MaterialPreviewGeometry>();
    geometry->m_meshes = std::move(meshes);
    const uint32_t meshCount = static_cast<uint32_t>(geometry->m_meshes.size());

    // Every mesh's positions and triangles one after another; a mesh with an index past its
    // vertices is left out (it has no triangles).
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> indices;
    geometry->m_meshFirstTriangle.resize(meshCount, 0);
    for (uint32_t index = 0; index < meshCount; ++index)
    {
        geometry->m_meshFirstTriangle[index] = static_cast<uint32_t>(indices.size() / 3);
        const MaterialPreviewMesh& entry = geometry->m_meshes[index];
        if (!entry.mesh || !entry.mesh->IsValid())
        {
            continue;
        }
        const MeshData& mesh = *entry.mesh;
        const uint32_t vertexCount = static_cast<uint32_t>(mesh.vertices.size());
        if (std::any_of(mesh.indices.begin(), mesh.indices.end(), [vertexCount](uint32_t vertex)
                        {
                            return vertex >= vertexCount;
                        }))
        {
            continue;
        }
        const uint32_t base = static_cast<uint32_t>(positions.size());
        const std::vector<glm::vec3> meshPositions = GatherPositions(mesh.vertices.front().position, mesh.vertices.size(), sizeof(Vertex));
        positions.insert(positions.end(), meshPositions.begin(), meshPositions.end());
        const size_t triangles = mesh.indices.size() / 3;
        for (size_t corner = 0; corner < triangles * 3; ++corner)
        {
            indices.push_back(base + mesh.indices[corner]);
        }
        geometry->m_triangleMesh.insert(geometry->m_triangleMesh.end(), triangles, index);
        for (const glm::vec3& position : meshPositions)
        {
            geometry->m_boundsMin = glm::min(geometry->m_boundsMin, position);
            geometry->m_boundsMax = glm::max(geometry->m_boundsMax, position);
        }
    }
    if (indices.empty())
    {
        return geometry;
    }
    MeshBvh bvh = BuildMeshBvh(positions, indices);
    geometry->m_nodes = std::move(bvh.nodes);
    geometry->m_triangles = std::move(bvh.triangles);
    geometry->m_sourceTriangles = std::move(bvh.sourceTriangles);
    return geometry;
}
}
