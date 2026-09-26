#include <engine/asset/model_loader.h>

#include <glm/glm.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

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

bool Near(float a, float b, float tolerance = 1e-4f)
{
    return std::abs(a - b) <= tolerance;
}

struct ScopedFixtureDirectory
{
    ScopedFixtureDirectory()
    {
        std::random_device randomDevice;
        path = std::filesystem::temp_directory_path() /
               ("miniengine_gltf_loading_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                std::to_string(randomDevice()));
        std::filesystem::create_directories(path);
    }

    ~ScopedFixtureDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

// One float triangle, and whatever the caller adds to the root object.
std::filesystem::path WriteTriangle(const std::filesystem::path& directory, const std::string& name, const std::string& extraRootMembers)
{
    const std::filesystem::path path = directory / (name + ".gltf");
    std::ofstream(path) << R"({ "asset": { "version": "2.0" }, )" << extraRootMembers << R"(
      "buffers": [{ "uri": ")"
                        << name << R"(.bin", "byteLength": 42 }],
      "bufferViews": [
        { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
        { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
      ],
      "accessors": [
        { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0] },
        { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
      ],
      "meshes": [{ "primitives": [{ "attributes": { "POSITION": 0 }, "indices": 1 }] }],
      "nodes": [{ "mesh": 0 }], "scenes": [{ "nodes": [0] }], "scene": 0 })";
    std::ofstream buffer(directory / (name + ".bin"), std::ios::binary);
    const std::array<float, 9> positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const std::array<uint16_t, 3> indices = {0, 1, 2};
    buffer.write(reinterpret_cast<const char*>(positions.data()), sizeof(positions));
    buffer.write(reinterpret_cast<const char*>(indices.data()), sizeof(indices));
    return path;
}

std::string LoadError(const std::filesystem::path& path)
{
    try
    {
        ModelLoader::LoadModel(path.string());
    }
    catch (const std::exception& error)
    {
        return error.what();
    }
    return {};
}

// A required extension the loader does not implement fails the import by name; one only used
// loads; the implemented ones pass as required.
void RequiredExtensionsAreChecked()
{
    const ScopedFixtureDirectory directory;
    const std::string unknown = LoadError(WriteTriangle(
        directory.path, "unknown", R"("extensionsUsed": ["EXT_made_up"], "extensionsRequired": ["EXT_made_up"],)"));
    Require(unknown.find("EXT_made_up") != std::string::npos, "a required unknown extension does not fail by name: '" + unknown + "'");

    Require(LoadError(WriteTriangle(directory.path, "used", R"("extensionsUsed": ["EXT_made_up"],)")).empty(),
            "an unknown extension that is only used fails the import");
    Require(LoadError(WriteTriangle(
                          directory.path,
                          "known",
                          R"("extensionsUsed": ["KHR_texture_transform", "KHR_materials_emissive_strength", "KHR_mesh_quantization"],
                             "extensionsRequired": ["KHR_texture_transform", "KHR_mesh_quantization"],)"))
                .empty(),
            "implemented extensions fail as required");
}

template <typename T>
void Append(std::vector<unsigned char>& bytes, std::initializer_list<T> values)
{
    for (const T value : values)
    {
        const size_t at = bytes.size();
        bytes.resize(at + sizeof(T));
        std::memcpy(bytes.data() + at, &value, sizeof(T));
    }
}

void Pad4(std::vector<unsigned char>& bytes)
{
    while (bytes.size() % 4 != 0)
    {
        bytes.push_back(0);
    }
}

// KHR_mesh_quantization: positions as unnormalized shorts, normals as normalized bytes, UVs as
// normalized unsigned shorts, each decoded as the specification says.
void QuantizedAttributesDecode()
{
    const ScopedFixtureDirectory directory;
    std::vector<unsigned char> bytes;
    Append<int16_t>(bytes, {0, 0, 0, 0, 100, 0, 0, 0, 0, 200, 0, 0}); // 3 x (x, y, z, pad) = 24 bytes
    const size_t normalOffset = bytes.size();
    Append<int8_t>(bytes, {0, 0, 127, 0, 0, 0, 127, 0, 0, 0, -127, 0}); // 3 x (x, y, z, pad) = 12 bytes
    const size_t uvOffset = bytes.size();
    Append<uint16_t>(bytes, {0, 0, 65535, 0, 0, 32768}); // 12 bytes
    const size_t indexOffset = bytes.size();
    Append<uint16_t>(bytes, {0, 1, 2});
    Pad4(bytes);
    std::ofstream(directory.path / "quantized.bin", std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    std::ofstream(directory.path / "quantized.gltf") << R"({ "asset": { "version": "2.0" },
      "extensionsUsed": ["KHR_mesh_quantization"], "extensionsRequired": ["KHR_mesh_quantization"],
      "buffers": [{ "uri": "quantized.bin", "byteLength": )"
                                                     << bytes.size() << R"( }],
      "bufferViews": [
        { "buffer": 0, "byteOffset": 0, "byteLength": 24, "byteStride": 8, "target": 34962 },
        { "buffer": 0, "byteOffset": )" << normalOffset
                                                     << R"(, "byteLength": 12, "byteStride": 4, "target": 34962 },
        { "buffer": 0, "byteOffset": )" << uvOffset << R"(, "byteLength": 12, "target": 34962 },
        { "buffer": 0, "byteOffset": )" << indexOffset
                                                     << R"(, "byteLength": 6, "target": 34963 }
      ],
      "accessors": [
        { "bufferView": 0, "componentType": 5122, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [100, 200, 0] },
        { "bufferView": 1, "componentType": 5120, "normalized": true, "count": 3, "type": "VEC3" },
        { "bufferView": 2, "componentType": 5123, "normalized": true, "count": 3, "type": "VEC2" },
        { "bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR" }
      ],
      "meshes": [{ "primitives": [{ "attributes": { "POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2 }, "indices": 3 }] }],
      "nodes": [{ "mesh": 0 }], "scenes": [{ "nodes": [0] }], "scene": 0 })";
    const LoadedModelData model = ModelLoader::LoadModel((directory.path / "quantized.gltf").string());
    const MeshData& mesh = model.submeshes.at(0).mesh;
    Require(mesh.vertices.size() == 3, "the quantized triangle has three vertices");
    // The loader may reorder nothing here: an identity node and counter-clockwise order.
    bool foundX = false;
    bool foundY = false;
    for (const Vertex& vertex : mesh.vertices)
    {
        foundX |= Near(vertex.position[0], 100.0f) && Near(vertex.position[1], 0.0f);
        foundY |= Near(vertex.position[0], 0.0f) && Near(vertex.position[1], 200.0f);
        Require(Near(std::abs(vertex.normal[2]), 1.0f), "a normalized byte 127 decodes to 1");
        if (Near(vertex.position[0], 100.0f))
        {
            Require(Near(vertex.texCoord[0], 1.0f), "a normalized unsigned short 65535 decodes to 1");
        }
        if (Near(vertex.position[1], 200.0f))
        {
            Require(Near(vertex.texCoord[1], 32768.0f / 65535.0f), "32768 decodes to 32768 / 65535");
        }
    }
    std::string seen;
    for (const Vertex& vertex : mesh.vertices)
    {
        seen += "(" + std::to_string(vertex.position[0]) + ", " + std::to_string(vertex.position[1]) + ", " + std::to_string(vertex.position[2]) + ") ";
    }
    Require(foundX && foundY, "unnormalized shorts decode to their integer values, not " + seen);
}
}

int main()
{
    try
    {
        RequiredExtensionsAreChecked();
        QuantizedAttributesDecode();
    }
    catch (const std::exception& error)
    {
        std::cerr << "glTF loading tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "glTF loading tests passed\n";
    return 0;
}
