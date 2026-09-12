#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace me
{

struct Vertex
{
    float position[3];
    float color[3];
    float texCoord[2];
    float normal[3];
    float tangent[4];
};

struct MeshData
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    bool IsValid() const
    {
        return !vertices.empty() && !indices.empty();
    }
};

MeshData CreateDefaultCubeMesh();

// Midpoint of the mesh's axis-aligned bounds, in the mesh's own space. Walks every
// vertex, so prefer the value a loader already cached (ModelSubmeshData::boundsCenter)
// over calling this on model geometry.
glm::vec3 ComputeMeshBoundsCenter(const MeshData& mesh);
}
