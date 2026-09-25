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
    // TEXCOORD_1, zero when the mesh has none. Textures read it through their transform's texCoord.
    float texCoord1[2];
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

// A sphere around the mesh, in the mesh's own space: centered on the midpoint of its
// axis-aligned bounds, with half their diagonal as the radius.
struct MeshBounds
{
    glm::vec3 center{0.0f};
    float radius = 0.0f;
};

// Walks every vertex, so prefer the values a loader already cached
// (ModelSubmeshData::boundsCenter and boundsRadius) over calling these on model geometry.
MeshBounds ComputeMeshBounds(const MeshData& mesh);
glm::vec3 ComputeMeshBoundsCenter(const MeshData& mesh);
}
