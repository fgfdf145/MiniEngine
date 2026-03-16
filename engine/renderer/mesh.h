#pragma once

#include <cstdint>
#include <vector>

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
