#include <engine/asset/model_loader.h>

#include <draco/compression/encode.h>
#include <draco/mesh/triangle_soup_mesh_builder.h>
#include <glm/glm.hpp>
#include <meshoptimizer.h>

#include <algorithm>
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
#include <sstream>
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
      "nodes": )" << nodes
                        << R"(, "scenes": [{ "nodes": [0] }], "scene": 0 })";
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

// A mesh's triangles as corner positions (and colours), each triangle rotated to start at its
// smallest corner and the list sorted: equal for two meshes that draw the same triangles in any
// vertex order, as a decoder is free to produce.
std::vector<std::array<float, 18>> CanonicalTriangles(const MeshData& mesh)
{
    std::vector<std::array<float, 18>> triangles;
    for (size_t index = 0; index + 2 < mesh.indices.size(); index += 3)
    {
        std::array<std::array<float, 6>, 3> corners{};
        for (size_t corner = 0; corner < 3; ++corner)
        {
            const Vertex& vertex = mesh.vertices.at(mesh.indices[index + corner]);
            for (size_t component = 0; component < 3; ++component)
            {
                // Rounded to a hundredth so a decoder's quantization does not change the order.
                corners[corner][component] = std::round(vertex.position[component] * 100.0f) / 100.0f;
                corners[corner][3 + component] = std::round(vertex.color[component] * 100.0f) / 100.0f;
            }
        }
        const size_t first = static_cast<size_t>(std::min_element(corners.begin(), corners.end()) - corners.begin());
        std::array<float, 18> triangle{};
        for (size_t corner = 0; corner < 3; ++corner)
        {
            std::copy(corners[(first + corner) % 3].begin(), corners[(first + corner) % 3].end(), triangle.begin() + corner * 6);
        }
        triangles.push_back(triangle);
    }
    std::sort(triangles.begin(), triangles.end());
    return triangles;
}

std::string Describe(const std::vector<std::array<float, 18>>& triangles)
{
    std::ostringstream text;
    for (const std::array<float, 18>& triangle : triangles)
    {
        text << "[";
        for (size_t corner = 0; corner < 3; ++corner)
        {
            text << " (" << triangle[corner * 6] << " " << triangle[corner * 6 + 1] << " " << triangle[corner * 6 + 2] << ")";
        }
        text << " ]";
    }
    return text.str();
}

void WriteBytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes)
{
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// A .glb of json and one BIN chunk.
void WriteGlb(const std::filesystem::path& path, std::string json, std::vector<unsigned char> bin)
{
    while (json.size() % 4 != 0)
    {
        json.push_back(' ');
    }
    Pad4(bin);
    std::vector<unsigned char> glb;
    Append<uint32_t>(glb, {0x46546C67u, 2u, static_cast<uint32_t>(12 + 8 + json.size() + 8 + bin.size())});
    Append<uint32_t>(glb, {static_cast<uint32_t>(json.size()), 0x4E4F534Au});
    glb.insert(glb.end(), json.begin(), json.end());
    Append<uint32_t>(glb, {static_cast<uint32_t>(bin.size()), 0x004E4942u});
    glb.insert(glb.end(), bin.begin(), bin.end());
    WriteBytes(path, glb);
}

// A quad of two triangles with normals, for the compressed geometry tests.
constexpr std::array<float, 12> kQuadPositions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f};
constexpr std::array<uint32_t, 6> kQuadIndices = {0, 1, 2, 0, 2, 3};

MeshData QuadMesh()
{
    MeshData mesh;
    for (size_t vertex = 0; vertex < 4; ++vertex)
    {
        Vertex corner{};
        std::copy_n(kQuadPositions.begin() + vertex * 3, 3, corner.position);
        corner.color[0] = corner.color[1] = corner.color[2] = 1.0f;
        mesh.vertices.push_back(corner);
    }
    mesh.indices.assign(kQuadIndices.begin(), kQuadIndices.end());
    return mesh;
}

// EXT_meshopt_compression and KHR_meshopt_compression: buffer views encoded with meshoptimizer's
// own encoders (positions as attributes, normals through the octahedral filter, triangles) decode
// back to the source, in a .gltf and in a .glb, with the data-less fallback buffer never read.
void MeshoptBufferViewsDecode()
{
    const ScopedFixtureDirectory directory;
    std::vector<unsigned char> compressed;
    const auto appendEncoded = [&compressed](const std::vector<unsigned char>& encoded)
    {
        Pad4(compressed);
        const size_t offset = compressed.size();
        compressed.insert(compressed.end(), encoded.begin(), encoded.end());
        return std::pair<size_t, size_t>{offset, encoded.size()};
    };

    std::vector<unsigned char> encoded(meshopt_encodeVertexBufferBound(4, 12));
    encoded.resize(meshopt_encodeVertexBuffer(encoded.data(), encoded.size(), kQuadPositions.data(), 4, 12));
    const auto [positionOffset, positionLength] = appendEncoded(encoded);

    const glm::vec3 normal = glm::normalize(glm::vec3(0.3f, -0.2f, 1.0f));
    const std::array<float, 16> normals = {normal.x, normal.y, normal.z, 0.0f, normal.x, normal.y, normal.z, 0.0f,
                                           normal.x, normal.y, normal.z, 0.0f, normal.x, normal.y, normal.z, 0.0f};
    std::array<int8_t, 16> filtered{};
    meshopt_encodeFilterOct(filtered.data(), 4, 4, 8, normals.data());
    encoded.assign(meshopt_encodeVertexBufferBound(4, 4), 0);
    encoded.resize(meshopt_encodeVertexBuffer(encoded.data(), encoded.size(), filtered.data(), 4, 4));
    const auto [normalOffset, normalLength] = appendEncoded(encoded);

    encoded.assign(meshopt_encodeIndexBufferBound(6, 4), 0);
    encoded.resize(meshopt_encodeIndexBuffer(encoded.data(), encoded.size(), kQuadIndices.data(), 6));
    const auto [indexOffset, indexLength] = appendEncoded(encoded);
    Pad4(compressed);

    // The fallback buffer is larger than the compressed data, as real files have it: tinygltf would
    // read it from a .glb's BIN chunk, which is too short, if the loader did not replace it.
    const auto json = [&](const std::string& compressedBuffer)
    {
        std::ostringstream text;
        text << R"({ "asset": { "version": "2.0" },
          "extensionsUsed": ["EXT_meshopt_compression", "KHR_meshopt_compression", "KHR_mesh_quantization"],
          "extensionsRequired": ["EXT_meshopt_compression", "KHR_meshopt_compression", "KHR_mesh_quantization"],
          "buffers": [)"
             << compressedBuffer << R"(,
            { "byteLength": 4096, "extensions": { "EXT_meshopt_compression": { "fallback": true } } }],
          "bufferViews": [
            { "buffer": 1, "byteOffset": 0, "byteLength": 48, "byteStride": 12, "target": 34962,
              "extensions": { "EXT_meshopt_compression": { "buffer": 0, "byteOffset": )"
             << positionOffset << R"(, "byteLength": )" << positionLength << R"(, "byteStride": 12, "count": 4, "mode": "ATTRIBUTES" } } },
            { "buffer": 1, "byteOffset": 48, "byteLength": 16, "byteStride": 4, "target": 34962,
              "extensions": { "KHR_meshopt_compression": { "buffer": 0, "byteOffset": )"
             << normalOffset << R"(, "byteLength": )" << normalLength << R"(, "byteStride": 4, "count": 4, "mode": "ATTRIBUTES", "filter": "OCTAHEDRAL" } } },
            { "buffer": 1, "byteOffset": 64, "byteLength": 12, "target": 34963,
              "extensions": { "EXT_meshopt_compression": { "buffer": 0, "byteOffset": )"
             << indexOffset << R"(, "byteLength": )" << indexLength << R"(, "byteStride": 2, "count": 6, "mode": "TRIANGLES" } } }
          ],
          "accessors": [
            { "bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0] },
            { "bufferView": 1, "componentType": 5120, "normalized": true, "count": 4, "type": "VEC3" },
            { "bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR" }
          ],
          "meshes": [{ "primitives": [{ "attributes": { "POSITION": 0, "NORMAL": 1 }, "indices": 2 }] }],
          "nodes": [{ "mesh": 0 }], "scenes": [{ "nodes": [0] }], "scene": 0 })";
        return text.str();
    };

    WriteBytes(directory.path / "meshopt.bin", compressed);
    std::ofstream(directory.path / "meshopt.gltf") << json(R"({ "uri": "meshopt.bin", "byteLength": )" + std::to_string(compressed.size()) + " }");
    WriteGlb(directory.path / "meshopt.glb", json(R"({ "byteLength": )" + std::to_string(compressed.size()) + " }"), compressed);

    for (const char* file : {"meshopt.gltf", "meshopt.glb"})
    {
        const LoadedModelData model = ModelLoader::LoadModel((directory.path / file).string());
        const MeshData& mesh = model.submeshes.at(0).mesh;
        Require(CanonicalTriangles(mesh) == CanonicalTriangles(QuadMesh()),
                std::string(file) + ": meshopt triangles decode to the source quad, not " + Describe(CanonicalTriangles(mesh)));
        for (const Vertex& vertex : mesh.vertices)
        {
            Require(glm::length(glm::vec3(vertex.normal[0], vertex.normal[1], vertex.normal[2]) - normal) < 0.02f,
                    std::string(file) + ": an octahedral-filtered normal decodes to the source normal");
        }
    }
}

// KHR_draco_mesh_compression: a mesh encoded with draco's encoder (quantized float positions, a
// normalized unsigned byte colour) decodes to the same triangles, whatever order draco puts its
// points and faces in.
void DracoPrimitivesDecode()
{
    const ScopedFixtureDirectory directory;
    draco::TriangleSoupMeshBuilder builder;
    builder.Start(2);
    const int positionAttribute = builder.AddAttribute(draco::GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
    const int colorAttribute = builder.AddAttribute(draco::GeometryAttribute::COLOR, 4, draco::DT_UINT8, true);
    const std::array<std::array<uint8_t, 4>, 4> colors = {{{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}}};
    for (uint32_t face = 0; face < 2; ++face)
    {
        const uint32_t* corners = kQuadIndices.data() + face * 3;
        builder.SetAttributeValuesForFace(
            positionAttribute, draco::FaceIndex(face), kQuadPositions.data() + corners[0] * 3, kQuadPositions.data() + corners[1] * 3, kQuadPositions.data() + corners[2] * 3);
        builder.SetAttributeValuesForFace(
            colorAttribute, draco::FaceIndex(face), colors[corners[0]].data(), colors[corners[1]].data(), colors[corners[2]].data());
    }
    const std::unique_ptr<draco::Mesh> source = builder.Finalize();
    Require(source != nullptr, "draco builds the source quad");
    draco::Encoder encoder;
    encoder.SetAttributeQuantization(draco::GeometryAttribute::POSITION, 14);
    draco::EncoderBuffer compressed;
    Require(encoder.EncodeMeshToBuffer(*source, &compressed).ok(), "draco encodes the source quad");
    std::vector<unsigned char> bytes(compressed.data(), compressed.data() + compressed.size());
    Pad4(bytes);
    WriteBytes(directory.path / "draco.bin", bytes);

    std::ofstream(directory.path / "draco.gltf") << R"({ "asset": { "version": "2.0" },
      "extensionsUsed": ["KHR_draco_mesh_compression"], "extensionsRequired": ["KHR_draco_mesh_compression"],
      "buffers": [{ "uri": "draco.bin", "byteLength": )"
                                                 << bytes.size() << R"( }],
      "bufferViews": [{ "buffer": 0, "byteOffset": 0, "byteLength": )"
                                                 << compressed.size() << R"( }],
      "accessors": [
        { "componentType": 5126, "count": )" << source->num_points()
                                                 << R"(, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0] },
        { "componentType": 5121, "normalized": true, "count": )"
                                                 << source->num_points() << R"(, "type": "VEC4" },
        { "componentType": 5123, "count": 6, "type": "SCALAR" }
      ],
      "meshes": [{ "primitives": [{ "attributes": { "POSITION": 0, "COLOR_0": 1 }, "indices": 2,
        "extensions": { "KHR_draco_mesh_compression": { "bufferView": 0, "attributes": { "POSITION": )"
                                                 << source->attribute(positionAttribute)->unique_id() << R"(, "COLOR_0": )"
                                                 << source->attribute(colorAttribute)->unique_id() << R"( } } } }] }],
      "nodes": [{ "mesh": 0 }], "scenes": [{ "nodes": [0] }], "scene": 0 })";

    MeshData expected = QuadMesh();
    for (size_t vertex = 0; vertex < 4; ++vertex)
    {
        for (size_t component = 0; component < 3; ++component)
        {
            expected.vertices[vertex].color[component] = colors[vertex][component] / 255.0f;
        }
    }
    const LoadedModelData model = ModelLoader::LoadModel((directory.path / "draco.gltf").string());
    const MeshData& mesh = model.submeshes.at(0).mesh;
    Require(CanonicalTriangles(mesh) == CanonicalTriangles(expected),
            "draco triangles decode to the source quad, not " + Describe(CanonicalTriangles(mesh)));
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
        MeshoptBufferViewsDecode();
        DracoPrimitivesDecode();
    }
    catch (const std::exception& error)
    {
        std::cerr << "glTF loading tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "glTF loading tests passed\n";
    return 0;
}
