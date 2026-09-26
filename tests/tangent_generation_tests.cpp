#include <engine/asset/model_loader.h>

#include <glm/glm.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::string Text(const glm::vec3& value)
{
    return "(" + std::to_string(value.x) + ", " + std::to_string(value.y) + ", " + std::to_string(value.z) + ")";
}

// A 2 x 2 m quad in the XY plane facing +Z, with no TANGENT attribute, textured the glTF way: UV
// (0, 0) is the image's top-left corner, so the image's up is +Y. mirrored runs u right to left.
LoadedModelData LoadQuad(const std::filesystem::path& directory, bool mirrored)
{
    const std::string name = mirrored ? "mirrored" : "quad";
    {
        std::ofstream file(directory / (name + ".gltf"));
        file << R"({ "asset": { "version": "2.0" },
          "buffers": [{ "uri": ")"
             << name << R"(.bin", "byteLength": 140 }],
          "bufferViews": [
            { "buffer": 0, "byteOffset": 0, "byteLength": 48, "target": 34962 },
            { "buffer": 0, "byteOffset": 48, "byteLength": 48, "target": 34962 },
            { "buffer": 0, "byteOffset": 96, "byteLength": 32, "target": 34962 },
            { "buffer": 0, "byteOffset": 128, "byteLength": 12, "target": 34963 }
          ],
          "accessors": [
            { "bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [-1, -1, 0], "max": [1, 1, 0] },
            { "bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3" },
            { "bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2" },
            { "bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR" }
          ],
          "meshes": [{ "primitives": [{ "attributes": { "POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2 }, "indices": 3 }] }],
          "nodes": [{ "mesh": 0 }], "scenes": [{ "nodes": [0] }], "scene": 0 })";
    }
    const std::array<float, 12> positions = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
    const std::array<float, 12> normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    std::array<float, 8> uvs{};
    for (size_t vertex = 0; vertex < 4; ++vertex)
    {
        const float x = positions[vertex * 3];
        const float y = positions[vertex * 3 + 1];
        uvs[vertex * 2] = mirrored ? (1.0f - x) * 0.5f : (x + 1.0f) * 0.5f;
        uvs[vertex * 2 + 1] = (1.0f - y) * 0.5f;
    }
    const std::array<uint16_t, 6> indices = {0, 1, 2, 0, 2, 3};
    std::ofstream buffer(directory / (name + ".bin"), std::ios::binary);
    buffer.write(reinterpret_cast<const char*>(positions.data()), sizeof(positions));
    buffer.write(reinterpret_cast<const char*>(normals.data()), sizeof(normals));
    buffer.write(reinterpret_cast<const char*>(uvs.data()), sizeof(uvs));
    buffer.write(reinterpret_cast<const char*>(indices.data()), sizeof(indices));
    buffer.close();
    return ModelLoader::LoadModel((directory / (name + ".gltf")).string());
}

// glTF's tangent space has +X along increasing u and +Y towards the image's top, which is
// decreasing v: the bitangent the shaders build, cross(N, T) * w, must point there. The Khronos
// Sample Viewer (cross(N, T) for untangented meshes) and MikkTSpace on an exporter's V-up UVs agree.
void GeneratedFrameFollowsTheImage(bool mirrored)
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("miniengine_tangent_generation_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const LoadedModelData model = LoadQuad(directory, mirrored);
    std::filesystem::remove_all(directory);

    const char* label = mirrored ? "mirrored quad" : "quad";
    for (const Vertex& vertex : model.submeshes.at(0).mesh.vertices)
    {
        const glm::vec3 normal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
        const glm::vec3 tangent(vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]);
        const glm::vec3 bitangent = glm::cross(normal, tangent) * vertex.tangent[3];
        const glm::vec3 expectedTangent(mirrored ? -1.0f : 1.0f, 0.0f, 0.0f);
        Require(glm::length(tangent - expectedTangent) < 1e-4f,
                std::string(label) + ": the tangent follows increasing u, " + Text(tangent));
        Require(glm::length(bitangent - glm::vec3(0.0f, 1.0f, 0.0f)) < 1e-4f,
                std::string(label) + ": the bitangent points at the image's top (+Y), not " + Text(bitangent));
    }
}
}

namespace
{
// A tent: two triangles sharing the edge along Y at the ridge, sloping down to -X and +X, with no
// NORMAL attribute. The four corners' UVs follow x and y so a copied vertex can be traced.
LoadedModelData LoadTentWithoutNormals(const std::filesystem::path& directory)
{
    {
        std::ofstream file(directory / "tent.gltf");
        file << R"({ "asset": { "version": "2.0" },
          "buffers": [{ "uri": "tent.bin", "byteLength": 92 }],
          "bufferViews": [
            { "buffer": 0, "byteOffset": 0, "byteLength": 48, "target": 34962 },
            { "buffer": 0, "byteOffset": 48, "byteLength": 32, "target": 34962 },
            { "buffer": 0, "byteOffset": 80, "byteLength": 12, "target": 34963 }
          ],
          "accessors": [
            { "bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [-1, -1, 0], "max": [1, 1, 1] },
            { "bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC2" },
            { "bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR" }
          ],
          "meshes": [{ "primitives": [{ "attributes": { "POSITION": 0, "TEXCOORD_0": 1 }, "indices": 2 }] }],
          "nodes": [{ "mesh": 0 }], "scenes": [{ "nodes": [0] }], "scene": 0 })";
    }
    // 0 and 1 are the ridge; 2 is the -X eave, 3 the +X eave.
    const std::array<float, 12> positions = {0, -1, 1, 0, 1, 1, -1, 0, 0, 1, 0, 0};
    const std::array<float, 8> uvs = {0.5f, 1.0f, 0.5f, 0.0f, 0.0f, 0.5f, 1.0f, 0.5f};
    const std::array<uint16_t, 6> indices = {0, 1, 2, 0, 3, 1};
    std::ofstream buffer(directory / "tent.bin", std::ios::binary);
    buffer.write(reinterpret_cast<const char*>(positions.data()), sizeof(positions));
    buffer.write(reinterpret_cast<const char*>(uvs.data()), sizeof(uvs));
    buffer.write(reinterpret_cast<const char*>(indices.data()), sizeof(indices));
    buffer.close();
    return ModelLoader::LoadModel((directory / "tent.gltf").string());
}

glm::vec3 Position(const Vertex& vertex)
{
    return glm::vec3(vertex.position[0], vertex.position[1], vertex.position[2]);
}

// glTF: "When normals are not specified, client implementations MUST calculate flat normals." Each
// triangle's corners carry its own face normal, so the ridge is a hard edge, not an averaged one.
void MissingNormalsAreFlat()
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("miniengine_flat_normals_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const LoadedModelData model = LoadTentWithoutNormals(directory);
    std::filesystem::remove_all(directory);

    const MeshData& mesh = model.submeshes.at(0).mesh;
    Require(mesh.indices.size() == 6, "the tent does not keep its two triangles");
    for (size_t index = 0; index + 2 < mesh.indices.size(); index += 3)
    {
        const Vertex& a = mesh.vertices.at(mesh.indices[index]);
        const Vertex& b = mesh.vertices.at(mesh.indices[index + 1]);
        const Vertex& c = mesh.vertices.at(mesh.indices[index + 2]);
        const glm::vec3 face = glm::normalize(glm::cross(Position(b) - Position(a), Position(c) - Position(a)));
        for (const Vertex* vertex : {&a, &b, &c})
        {
            const glm::vec3 normal(vertex->normal[0], vertex->normal[1], vertex->normal[2]);
            Require(glm::length(normal - face) < 1e-4f, "a corner's normal is " + Text(normal) + ", not its face's " + Text(face));
            // Copied with its UV: u follows x, v follows -y.
            Require(std::abs(vertex->texCoord[0] - (vertex->position[0] + 1.0f) * 0.5f) < 1e-6f &&
                        std::abs(vertex->texCoord[1] - (1.0f - vertex->position[1]) * 0.5f) < 1e-6f,
                    "a split corner lost its UV");
        }
    }
}

// A mesh that has normals keeps its shared vertices.
void GivenNormalsKeepSharedVertices()
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("miniengine_shared_normals_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const LoadedModelData model = LoadQuad(directory, false);
    std::filesystem::remove_all(directory);
    Require(model.submeshes.at(0).mesh.vertices.size() == 4, "a quad with normals was split into " +
                                                                 std::to_string(model.submeshes.at(0).mesh.vertices.size()) + " vertices");
}
}

int main()
{
    try
    {
        GeneratedFrameFollowsTheImage(false);
        GeneratedFrameFollowsTheImage(true);
        MissingNormalsAreFlat();
        GivenNormalsKeepSharedVertices();
    }
    catch (const std::exception& error)
    {
        std::cerr << "tangent generation tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "tangent generation tests passed\n";
    return 0;
}
