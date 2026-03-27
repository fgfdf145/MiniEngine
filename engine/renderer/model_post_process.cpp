#include "model_post_process.h"

#include <glm/geometric.hpp>

#include <cmath>
#include <limits>
#include <vector>

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

void GenerateNormals(MeshData& meshData)
{
    for (Vertex& vertex : meshData.vertices)
    {
        vertex.normal[0] = 0.0f;
        vertex.normal[1] = 0.0f;
        vertex.normal[2] = 0.0f;
    }

    for (size_t index = 0; index + 2 < meshData.indices.size(); index += 3)
    {
        Vertex& vertex0 = meshData.vertices[meshData.indices[index + 0]];
        Vertex& vertex1 = meshData.vertices[meshData.indices[index + 1]];
        Vertex& vertex2 = meshData.vertices[meshData.indices[index + 2]];

        const glm::vec3 position0(vertex0.position[0], vertex0.position[1], vertex0.position[2]);
        const glm::vec3 position1(vertex1.position[0], vertex1.position[1], vertex1.position[2]);
        const glm::vec3 position2(vertex2.position[0], vertex2.position[1], vertex2.position[2]);

        const glm::vec3 edge01 = position1 - position0;
        const glm::vec3 edge02 = position2 - position0;
        const glm::vec3 faceNormal = glm::cross(edge01, edge02);
        if (glm::length(faceNormal) <= std::numeric_limits<float>::epsilon())
        {
            continue;
        }

        const glm::vec3 normalizedFaceNormal = glm::normalize(faceNormal);
        for (Vertex* vertex : { &vertex0, &vertex1, &vertex2 })
        {
            vertex->normal[0] += normalizedFaceNormal.x;
            vertex->normal[1] += normalizedFaceNormal.y;
            vertex->normal[2] += normalizedFaceNormal.z;
        }
    }

    for (Vertex& vertex : meshData.vertices)
    {
        glm::vec3 normal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
        if (glm::length(normal) <= std::numeric_limits<float>::epsilon())
        {
            normal = glm::vec3(0.0f, 1.0f, 0.0f);
        }
        else
        {
            normal = glm::normalize(normal);
        }

        vertex.normal[0] = normal.x;
        vertex.normal[1] = normal.y;
        vertex.normal[2] = normal.z;
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
        const glm::vec3 triangleBitangent =
            (edge02 * deltaUv01.x - edge01 * deltaUv02.x) * inverseDeterminant;

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
        GenerateNormals(submeshData.mesh);
        submeshData.hasNormals = true;
    }

    if (!submeshData.hasTangents)
    {
        GenerateTangents(submeshData.mesh, submeshData.hasTexCoords);
        submeshData.hasTangents = true;
    }
}
