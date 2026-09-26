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
std::filesystem::path WriteTriangle(
    const std::filesystem::path& directory,
    const std::string& name,
    const std::string& extraRootMembers,
    const std::string& nodes = R"([{ "mesh": 0 }])")
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
      "nodes": )" << nodes << R"(, "scenes": [{ "nodes": [0] }], "scene": 0 })";
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

const ModelLightData& LightNamed(const LoadedModelData& model, const std::string& name)
{
    for (const ModelLightData& light : model.lights)
    {
        if (light.name == name)
        {
            return light;
        }
    }
    throw std::runtime_error("no light named " + name);
}

bool Near3(const glm::vec3& a, const glm::vec3& b)
{
    return glm::length(a - b) < 1e-4f;
}

// KHR_lights_punctual: each light with its node's world transform, in the engine's units (glTF's
// candela becomes lumens over the light's solid angle; lux stays lux), an undefined range made finite.
void PunctualLightsImport()
{
    const ScopedFixtureDirectory directory;
    const LoadedModelData model = ModelLoader::LoadModel(
        WriteTriangle(
            directory.path,
            "lights",
            R"("extensionsUsed": ["KHR_lights_punctual"],
               "extensions": { "KHR_lights_punctual": { "lights": [
                 { "name": "sun", "type": "directional", "color": [1, 0.5, 0.25], "intensity": 2 },
                 { "name": "bulb", "type": "point", "intensity": 10 },
                 { "name": "cone", "type": "spot", "intensity": 5, "range": 4, "spot": { "innerConeAngle": 0.2, "outerConeAngle": 0.5 } },
                 { "name": "plain", "type": "spot", "spot": {} } ] } },)",
            R"([{ "mesh": 0, "children": [1, 2, 3, 4] },
                { "rotation": [-0.70710678, 0, 0, 0.70710678], "extensions": { "KHR_lights_punctual": { "light": 0 } } },
                { "translation": [1, 2, 3], "extensions": { "KHR_lights_punctual": { "light": 1 } } },
                { "translation": [0, 1, 0], "extensions": { "KHR_lights_punctual": { "light": 2 } } },
                { "extensions": { "KHR_lights_punctual": { "light": 3 } } }])")
            .string());
    Require(model.lights.size() == 4, "four lights, not " + std::to_string(model.lights.size()));

    const ModelLightData& sun = LightNamed(model, "sun");
    Require(sun.type == LightType::Directional, "the sun is directional");
    Require(Near3(sun.direction, glm::vec3(0.0f, -1.0f, 0.0f)), "the sun shines down its node's -Z");
    Require(Near(sun.intensity, 2.0f), "directional lux stays lux");
    Require(Near3(sun.color, glm::vec3(1.0f, 0.5f, 0.25f)), "the colour");

    const ModelLightData& bulb = LightNamed(model, "bulb");
    Require(bulb.type == LightType::Point, "the bulb is a point light");
    Require(Near3(bulb.position, glm::vec3(1.0f, 2.0f, 3.0f)), "at its node");
    Require(Near(bulb.intensity, 10.0f * 4.0f * 3.14159265f, 1e-3f), "10 cd is 40 pi lumens");
    Require(Near(bulb.range, 100.0f, 1e-3f), "an undefined range ends where the light falls to 1e-3 lux");

    const ModelLightData& cone = LightNamed(model, "cone");
    Require(cone.type == LightType::Spot, "the cone is a spot light");
    Require(Near3(cone.direction, glm::vec3(0.0f, 0.0f, -1.0f)), "a spot shines down -Z");
    Require(Near(cone.range, 4.0f), "a given range is kept");
    Require(Near(cone.innerAngleDegrees, 0.2f * 57.2957795f, 1e-3f) && Near(cone.outerAngleDegrees, 0.5f * 57.2957795f, 1e-3f), "the cone");
    Require(Near(cone.intensity, 5.0f * 2.0f * 3.14159265f * (1.0f - std::cos(0.5f)), 1e-3f), "candela to lumens over the outer cone");

    const ModelLightData& plain = LightNamed(model, "plain");
    Require(Near(plain.innerAngleDegrees, 0.0f) && Near(plain.outerAngleDegrees, 45.0f, 1e-3f), "glTF's default cone");
    Require(Near(plain.intensity, 2.0f * 3.14159265f * (1.0f - std::cos(3.14159265f / 4.0f)), 1e-3f), "glTF's default intensity of 1 cd");
}
}

int main()
{
    try
    {
        RequiredExtensionsAreChecked();
        QuantizedAttributesDecode();
        PunctualLightsImport();
    }
    catch (const std::exception& error)
    {
        std::cerr << "glTF loading tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "glTF loading tests passed\n";
    return 0;
}
