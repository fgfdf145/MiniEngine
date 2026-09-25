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

int main()
{
    try
    {
        GeneratedFrameFollowsTheImage(false);
        GeneratedFrameFollowsTheImage(true);
    }
    catch (const std::exception& error)
    {
        std::cerr << "tangent generation tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "tangent generation tests passed\n";
    return 0;
}
