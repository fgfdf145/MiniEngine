#include "material_preview_shapes.h"

#include <glm/glm.hpp>

#include <cmath>

namespace me
{

namespace
{
constexpr float kPi = 3.14159265358979f;

uint32_t AddVertex(MeshData& mesh, const glm::vec3& position, const glm::vec3& normal, const glm::vec4& tangent, const glm::vec2& uv)
{
    Vertex vertex{};
    vertex.position[0] = position.x;
    vertex.position[1] = position.y;
    vertex.position[2] = position.z;
    vertex.color[0] = 1.0f;
    vertex.color[1] = 1.0f;
    vertex.color[2] = 1.0f;
    vertex.texCoord[0] = uv.x;
    vertex.texCoord[1] = uv.y;
    vertex.texCoord1[0] = uv.x;
    vertex.texCoord1[1] = uv.y;
    vertex.normal[0] = normal.x;
    vertex.normal[1] = normal.y;
    vertex.normal[2] = normal.z;
    vertex.tangent[0] = tangent.x;
    vertex.tangent[1] = tangent.y;
    vertex.tangent[2] = tangent.z;
    vertex.tangent[3] = tangent.w;
    mesh.vertices.push_back(vertex);
    return static_cast<uint32_t>(mesh.vertices.size() - 1);
}

// A grid of (columns + 1) x (rows + 1) vertices from point(u, v). Its triangles run
// counter-clockwise (glTF's front) seen from the side cross(dP/dv, dP/du) points to, or from the
// other side with `flip`.
template <typename Point>
void AddGrid(MeshData& mesh, uint32_t columns, uint32_t rows, bool flip, Point&& point)
{
    const uint32_t first = static_cast<uint32_t>(mesh.vertices.size());
    for (uint32_t row = 0; row <= rows; ++row)
    {
        for (uint32_t column = 0; column <= columns; ++column)
        {
            const glm::vec2 uv(static_cast<float>(column) / static_cast<float>(columns), static_cast<float>(row) / static_cast<float>(rows));
            glm::vec3 position;
            glm::vec3 normal;
            glm::vec4 tangent;
            point(uv, position, normal, tangent);
            AddVertex(mesh, position, normal, tangent, uv);
        }
    }
    for (uint32_t row = 0; row < rows; ++row)
    {
        for (uint32_t column = 0; column < columns; ++column)
        {
            const uint32_t a = first + row * (columns + 1) + column;
            const uint32_t b = a + 1;
            const uint32_t c = a + columns + 1;
            const uint32_t d = c + 1;
            if (flip)
            {
                mesh.indices.insert(mesh.indices.end(), {a, b, c, b, d, c});
            }
            else
            {
                mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
            }
        }
    }
}

// The tangent frame's handedness for a surface whose texture u runs along `tangent` and v along
// `vDirection`: the bitangent cross(N, T) * w is -dP/dv, as the engine's tangent generation makes it
// (ModelPostProcess: MikkTSpace on an exporter's V-up UVs).
float Handedness(const glm::vec3& normal, const glm::vec3& tangent, const glm::vec3& vDirection)
{
    return glm::dot(glm::cross(normal, tangent), -vDirection) >= 0.0f ? 1.0f : -1.0f;
}

// A flat face: centred at `centre`, with half-extents `right` (texture u) and `down` (texture v),
// facing cross(down, right).
void AddFace(MeshData& mesh, const glm::vec3& centre, const glm::vec3& right, const glm::vec3& down, uint32_t subdivisions)
{
    const glm::vec3 normal = glm::normalize(glm::cross(down, right));
    const glm::vec3 tangentDirection = glm::normalize(right);
    const float handedness = Handedness(normal, tangentDirection, glm::normalize(down));
    AddGrid(mesh, subdivisions, subdivisions, false, [&](const glm::vec2& uv, glm::vec3& position, glm::vec3& n, glm::vec4& tangent)
            {
                position = centre + right * (uv.x * 2.0f - 1.0f) + down * (uv.y * 2.0f - 1.0f);
                n = normal;
                tangent = glm::vec4(tangentDirection, handedness);
            });
}
}

const char* ToString(MaterialPreviewShape shape)
{
    switch (shape)
    {
    case MaterialPreviewShape::Sphere:
        return "Sphere";
    case MaterialPreviewShape::Cube:
        return "Cube";
    case MaterialPreviewShape::Plane:
        return "Plane";
    case MaterialPreviewShape::Cylinder:
        return "Cylinder";
    case MaterialPreviewShape::Model:
    default:
        return "Model";
    }
}

MeshData BuildMaterialPreviewShapeMesh(MaterialPreviewShape shape)
{
    MeshData mesh;
    switch (shape)
    {
    case MaterialPreviewShape::Sphere:
    {
        // u around from -Z through +X (the equirectangular layout), v from the top down: two wraps
        // of the texture around, one down, as Unreal's preview sphere maps it.
        constexpr float kRadius = 0.5f;
        // v runs down from the top, u around: dP/dv x dP/du points in, so the grid is flipped, and the
        // bitangent (up, -dP/dv) is -cross(N, T).
        AddGrid(mesh, 128, 64, true, [](const glm::vec2& uv, glm::vec3& position, glm::vec3& normal, glm::vec4& tangent)
                {
                    const float theta = uv.y * kPi;
                    const float phi = uv.x * 2.0f * kPi;
                    normal = glm::vec3(std::sin(theta) * std::sin(phi), std::cos(theta), -std::sin(theta) * std::cos(phi));
                    position = normal * kRadius;
                    tangent = glm::vec4(std::cos(phi), 0.0f, std::sin(phi), -1.0f);
                });
        for (Vertex& vertex : mesh.vertices)
        {
            vertex.texCoord[0] *= 2.0f;
            vertex.texCoord1[0] *= 2.0f;
        }
        break;
    }
    case MaterialPreviewShape::Cube:
    {
        constexpr float kHalf = 0.4f;
        const glm::vec3 x(kHalf, 0.0f, 0.0f);
        const glm::vec3 y(0.0f, kHalf, 0.0f);
        const glm::vec3 z(0.0f, 0.0f, kHalf);
        AddFace(mesh, z, x, -y, 8);   // +Z
        AddFace(mesh, -z, -x, -y, 8); // -Z
        AddFace(mesh, x, -z, -y, 8);  // +X
        AddFace(mesh, -x, z, -y, 8);  // -X
        AddFace(mesh, y, x, z, 8);    // +Y
        AddFace(mesh, -y, x, -z, 8);  // -Y
        break;
    }
    case MaterialPreviewShape::Plane:
    {
        AddFace(mesh, glm::vec3(0.0f), glm::vec3(0.5f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 0.5f), 16);
        break;
    }
    case MaterialPreviewShape::Cylinder:
    {
        constexpr float kRadius = 0.35f;
        constexpr float kHalfHeight = 0.45f;
        // Laid out as the sphere's side.
        AddGrid(mesh, 96, 16, true, [](const glm::vec2& uv, glm::vec3& position, glm::vec3& normal, glm::vec4& tangent)
                {
                    const float phi = uv.x * 2.0f * kPi;
                    normal = glm::vec3(std::sin(phi), 0.0f, -std::cos(phi));
                    position = normal * kRadius + glm::vec3(0.0f, kHalfHeight - uv.y * 2.0f * kHalfHeight, 0.0f);
                    tangent = glm::vec4(std::cos(phi), 0.0f, std::sin(phi), -1.0f);
                });
        // The caps: fans, the texture laid flat over each (u along +X, v along +Z on top and -Z
        // below), so the bitangent cross(N, +X) is -dP/dv on both: handedness 1.
        for (const float side : {1.0f, -1.0f})
        {
            const glm::vec3 normal(0.0f, side, 0.0f);
            const uint32_t first = static_cast<uint32_t>(mesh.vertices.size());
            constexpr uint32_t kSegments = 96;
            AddVertex(mesh, normal * kHalfHeight, normal, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f), glm::vec2(0.5f));
            for (uint32_t segment = 0; segment <= kSegments; ++segment)
            {
                const float phi = static_cast<float>(segment) / static_cast<float>(kSegments) * 2.0f * kPi;
                const glm::vec3 rim(std::sin(phi) * kRadius, side * kHalfHeight, -std::cos(phi) * kRadius);
                AddVertex(mesh, rim, normal, glm::vec4(1.0f, 0.0f, 0.0f, 1.0f), glm::vec2(0.5f + rim.x / (2.0f * kRadius), 0.5f + side * rim.z / (2.0f * kRadius)));
            }
            for (uint32_t segment = 0; segment < kSegments; ++segment)
            {
                const uint32_t a = first + 1 + segment;
                const uint32_t b = a + 1;
                // Counter-clockwise seen from above for the top, from below for the bottom.
                if (side > 0.0f)
                {
                    mesh.indices.insert(mesh.indices.end(), {first, b, a});
                }
                else
                {
                    mesh.indices.insert(mesh.indices.end(), {first, a, b});
                }
            }
        }
        break;
    }
    case MaterialPreviewShape::Model:
    default:
        break;
    }
    return mesh;
}
}
