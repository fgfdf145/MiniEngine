#include <engine/asset/model_loader.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
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

class ScopedFixtureDirectory
{
  public:
    ScopedFixtureDirectory()
    {
        std::random_device randomDevice;
        path = std::filesystem::temp_directory_path() /
               ("miniengine_gltf_material_extensions_" +
                std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()) + "_" +
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

// One triangle per material, so every material is used and loaded, in material order.
std::filesystem::path WriteFixture(const std::filesystem::path& directory)
{
    const std::filesystem::path path = directory / "emissive_strength.gltf";
    std::ofstream file(path);
    file << R"({
      "asset": { "version": "2.0" },
      "extensionsUsed": ["KHR_materials_emissive_strength"],
      "materials": [
        { "name": "float", "emissiveFactor": [1, 0.5, 0.25],
          "extensions": { "KHR_materials_emissive_strength": { "emissiveStrength": 4.0 } } },
        { "name": "integer", "emissiveFactor": [1, 1, 1],
          "extensions": { "KHR_materials_emissive_strength": { "emissiveStrength": 4 } } },
        { "name": "absent", "emissiveFactor": [1, 1, 1] },
        { "name": "negative", "emissiveFactor": [1, 1, 1],
          "extensions": { "KHR_materials_emissive_strength": { "emissiveStrength": -1.0 } } },
        { "name": "string", "emissiveFactor": [1, 1, 1],
          "extensions": { "KHR_materials_emissive_strength": { "emissiveStrength": "x" } } }
      ],
      "buffers": [{ "uri": "emissive_strength.bin", "byteLength": 42 }],
      "bufferViews": [
        { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
        { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
      ],
      "accessors": [
        { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
        { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
      ],
      "meshes": [{ "primitives": [
        { "attributes": { "POSITION": 0 }, "indices": 1, "material": 0 },
        { "attributes": { "POSITION": 0 }, "indices": 1, "material": 1 },
        { "attributes": { "POSITION": 0 }, "indices": 1, "material": 2 },
        { "attributes": { "POSITION": 0 }, "indices": 1, "material": 3 },
        { "attributes": { "POSITION": 0 }, "indices": 1, "material": 4 }
      ]}],
      "nodes": [{ "mesh": 0 }],
      "scenes": [{ "nodes": [0] }],
      "scene": 0
    })";
    file.close();

    std::ofstream buffer(directory / "emissive_strength.bin", std::ios::binary);
    const std::array<float, 9> positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const std::array<uint16_t, 3> indices = {0, 1, 2};
    buffer.write(reinterpret_cast<const char*>(positions.data()), static_cast<std::streamsize>(sizeof(positions)));
    buffer.write(reinterpret_cast<const char*>(indices.data()), static_cast<std::streamsize>(sizeof(indices)));
    return path;
}

float IntensityOf(const LoadedModelData& model, const std::string& name)
{
    for (const ModelMaterialData& material : model.materials)
    {
        if (material.name == name)
        {
            return material.emissiveIntensity;
        }
    }
    throw std::runtime_error("material '" + name + "' was not loaded");
}

void ReadsEmissiveStrength()
{
    const ScopedFixtureDirectory directory;
    const LoadedModelData model = ModelLoader::LoadModel(WriteFixture(directory.path).string());
    Require(model.IsValid(), "the fixture loads");

    const auto expect = [&](const std::string& name, float expected)
    {
        const float actual = IntensityOf(model, name);
        Require(std::fabs(actual - expected) < 1e-6f,
                name + ": expected emissive intensity " + std::to_string(expected) + ", got " + std::to_string(actual));
    };
    expect("float", 4.0f);
    // tinygltf keeps a JSON 4 as an integer; the extension allows any number.
    expect("integer", 4.0f);
    expect("absent", 1.0f);
    // Invalid strengths leave the default rather than darkening or inverting the emission.
    expect("negative", 1.0f);
    expect("string", 1.0f);
}
}

int main()
{
    try
    {
        ReadsEmissiveStrength();
    }
    catch (const std::exception& error)
    {
        std::cerr << "glTF material extension tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "glTF material extension tests passed\n";
    return 0;
}
