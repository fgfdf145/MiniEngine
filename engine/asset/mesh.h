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
    // The smoothed normal a toon outline is pushed along (MINIENGINE_toon's _SMOOTH_NORMAL), in the
    // same space as normal; zero when the mesh has none, and the toon passes fall back to normal.
    float outlineNormal[3];
};

// One vertex's skin (glTF JOINTS_0 / WEIGHTS_0): up to four joints of the submesh's skin binding
// (ModelSkinBinding) and their weights, which sum to 1. Laid out as skin.comp reads it.
struct VertexSkin
{
    uint16_t joints[4] = {0, 0, 0, 0};
    float weights[4] = {1.0f, 0.0f, 0.0f, 0.0f};
};

struct MeshData
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    // Parallel to vertices for a skinned mesh (ModelSubmeshData::skinBinding), empty otherwise: the
    // vertices are then the bind pose, which the skinning pass deforms each frame.
    std::vector<VertexSkin> skin;

    bool IsSkinned() const
    {
        return !skin.empty();
    }

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
// (ModelSubmeshData::boundsCenter and boundsRadius) over calling this on model geometry.
MeshBounds ComputeMeshBounds(const MeshData& mesh);
}
