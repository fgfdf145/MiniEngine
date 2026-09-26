#include "model_post_process.h"

#include <glm/geometric.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace me
{

namespace
{
glm::vec3 ChooseOrthogonalTangent(const glm::vec3& normal)
{
    const glm::vec3 up = std::abs(normal.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 tangent = glm::cross(up, normal);
    if (glm::length(tangent) <= std::numeric_limits<float>::epsilon())
    {
        tangent = glm::vec3(1.0f, 0.0f, 0.0f);
    }
    else
    {
        tangent = glm::normalize(tangent);
    }

    return tangent;
}

// glTF: "When normals are not specified, client implementations MUST calculate flat normals." Each
// triangle gets corners of its own, copies of the shared vertices, carrying its face normal; a
// degenerate triangle points up.
void GenerateFlatNormals(MeshData& meshData)
{
    std::vector<Vertex> vertices;
    vertices.reserve(meshData.indices.size());
    for (size_t index = 0; index + 2 < meshData.indices.size(); index += 3)
    {
        std::array<Vertex, 3> corners = {
            meshData.vertices[meshData.indices[index + 0]],
            meshData.vertices[meshData.indices[index + 1]],
            meshData.vertices[meshData.indices[index + 2]]};

        const glm::vec3 position0(corners[0].position[0], corners[0].position[1], corners[0].position[2]);
        const glm::vec3 position1(corners[1].position[0], corners[1].position[1], corners[1].position[2]);
        const glm::vec3 position2(corners[2].position[0], corners[2].position[1], corners[2].position[2]);
        const glm::vec3 faceNormal = glm::cross(position1 - position0, position2 - position0);
        const glm::vec3 normal = glm::length(faceNormal) <= std::numeric_limits<float>::epsilon()
                                     ? glm::vec3(0.0f, 1.0f, 0.0f)
                                     : glm::normalize(faceNormal);

        for (Vertex& corner : corners)
        {
            corner.normal[0] = normal.x;
            corner.normal[1] = normal.y;
            corner.normal[2] = normal.z;
            vertices.push_back(corner);
        }
    }

    meshData.vertices = std::move(vertices);
    for (size_t index = 0; index < meshData.indices.size(); ++index)
    {
        meshData.indices[index] = static_cast<uint32_t>(index);
    }
}

void GenerateTangents(MeshData& meshData, bool hasTexCoords)
{
    if (!hasTexCoords)
    {
        for (Vertex& vertex : meshData.vertices)
        {
            const glm::vec3 normal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
            const glm::vec3 tangent = ChooseOrthogonalTangent(normal);
            vertex.tangent[0] = tangent.x;
            vertex.tangent[1] = tangent.y;
            vertex.tangent[2] = tangent.z;
            vertex.tangent[3] = 1.0f;
        }
        return;
    }

    std::vector<glm::vec3> tangents(meshData.vertices.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> bitangents(meshData.vertices.size(), glm::vec3(0.0f));

    for (size_t index = 0; index + 2 < meshData.indices.size(); index += 3)
    {
        const uint32_t index0 = meshData.indices[index + 0];
        const uint32_t index1 = meshData.indices[index + 1];
        const uint32_t index2 = meshData.indices[index + 2];

        const Vertex& vertex0 = meshData.vertices[index0];
        const Vertex& vertex1 = meshData.vertices[index1];
        const Vertex& vertex2 = meshData.vertices[index2];

        const glm::vec3 position0(vertex0.position[0], vertex0.position[1], vertex0.position[2]);
        const glm::vec3 position1(vertex1.position[0], vertex1.position[1], vertex1.position[2]);
        const glm::vec3 position2(vertex2.position[0], vertex2.position[1], vertex2.position[2]);

        const glm::vec2 uv0(vertex0.texCoord[0], vertex0.texCoord[1]);
        const glm::vec2 uv1(vertex1.texCoord[0], vertex1.texCoord[1]);
        const glm::vec2 uv2(vertex2.texCoord[0], vertex2.texCoord[1]);

        const glm::vec3 edge01 = position1 - position0;
        const glm::vec3 edge02 = position2 - position0;
        const glm::vec2 deltaUv01 = uv1 - uv0;
        const glm::vec2 deltaUv02 = uv2 - uv0;

        const float determinant = deltaUv01.x * deltaUv02.y - deltaUv02.x * deltaUv01.y;
        if (std::abs(determinant) <= std::numeric_limits<float>::epsilon())
        {
            continue;
        }

        const float inverseDeterminant = 1.0f / determinant;
        const glm::vec3 triangleTangent =
            (edge01 * deltaUv02.y - edge02 * deltaUv01.y) * inverseDeterminant;
        // glTF's tangent-space +Y is the image's top, which is decreasing v (UV (0, 0) is the
        // top-left corner): the bitangent is -dP/dv, as MikkTSpace gives on an exporter's V-up UVs
        // and the Khronos Sample Viewer's cross(N, T) gives for unmirrored ones.
        const glm::vec3 triangleBitangent =
            -(edge02 * deltaUv01.x - edge01 * deltaUv02.x) * inverseDeterminant;

        tangents[index0] += triangleTangent;
        tangents[index1] += triangleTangent;
        tangents[index2] += triangleTangent;
        bitangents[index0] += triangleBitangent;
        bitangents[index1] += triangleBitangent;
        bitangents[index2] += triangleBitangent;
    }

    for (size_t vertexIndex = 0; vertexIndex < meshData.vertices.size(); ++vertexIndex)
    {
        Vertex& vertex = meshData.vertices[vertexIndex];
        const glm::vec3 normal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);

        glm::vec3 tangent = tangents[vertexIndex];
        if (glm::length(tangent) <= std::numeric_limits<float>::epsilon())
        {
            tangent = ChooseOrthogonalTangent(normal);
        }
        else
        {
            tangent = glm::normalize(tangent - normal * glm::dot(normal, tangent));
        }

        glm::vec3 bitangent = bitangents[vertexIndex];
        if (glm::length(bitangent) <= std::numeric_limits<float>::epsilon())
        {
            bitangent = glm::normalize(glm::cross(normal, tangent));
        }
        else
        {
            bitangent = glm::normalize(bitangent);
        }

        const float handedness = glm::dot(glm::cross(normal, tangent), bitangent) < 0.0f ? -1.0f : 1.0f;
        vertex.tangent[0] = tangent.x;
        vertex.tangent[1] = tangent.y;
        vertex.tangent[2] = tangent.z;
        vertex.tangent[3] = handedness;
    }
}
}

void ModelPostProcess::FinalizeSubmeshData(ModelSubmeshData& submeshData)
{
    if (!submeshData.mesh.IsValid())
    {
        return;
    }

    if (!submeshData.hasNormals)
    {
        GenerateFlatNormals(submeshData.mesh);
        submeshData.hasNormals = true;
    }

    if (!submeshData.hasTangents)
    {
        GenerateTangents(submeshData.mesh, submeshData.hasTexCoords);
        submeshData.hasTangents = true;
    }

    // Last, so it sees the final vertex set: the renderable built from this submesh reads
    // the cached value instead of walking the vertices again on the main thread.
    const MeshBounds bounds = ComputeMeshBounds(submeshData.mesh);
    submeshData.boundsCenter = bounds.center;
    submeshData.boundsRadius = bounds.radius;
}
}
