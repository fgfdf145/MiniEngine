#include <engine/asset/material_definition.h>
#include <engine/asset/model_loader.h>

#include <yaml-cpp/yaml.h>

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

// A glTF whose materials are the given JSON objects, one triangle per material so every one of them
// is used and loaded.
std::filesystem::path WriteModel(const std::filesystem::path& directory, const std::string& name, const std::vector<std::string>& materials)
{
    const std::filesystem::path path = directory / (name + ".gltf");
    std::string materialList;
    std::string primitives;
    for (size_t index = 0; index < materials.size(); ++index)
    {
        materialList += (index == 0 ? "" : ",") + materials[index];
        primitives += std::string(index == 0 ? "" : ",") + R"({ "attributes": { "POSITION": 0 }, "indices": 1, "material": )" +
                      std::to_string(index) + " }";
    }
    std::ofstream file(path);
    file << R"({ "asset": { "version": "2.0" }, "materials": [)" << materialList << R"(],
      "buffers": [{ "uri": ")"
         << name << R"(.bin", "byteLength": 42 }],
      "bufferViews": [
        { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
        { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
      ],
      "accessors": [
        { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
        { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
      ],
      "meshes": [{ "primitives": [)"
         << primitives << R"(]}],
      "nodes": [{ "mesh": 0 }], "scenes": [{ "nodes": [0] }], "scene": 0 })";
    file.close();

    std::ofstream buffer(directory / (name + ".bin"), std::ios::binary);
    const std::array<float, 9> positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const std::array<uint16_t, 3> indices = {0, 1, 2};
    buffer.write(reinterpret_cast<const char*>(positions.data()), static_cast<std::streamsize>(sizeof(positions)));
    buffer.write(reinterpret_cast<const char*>(indices.data()), static_cast<std::streamsize>(sizeof(indices)));
    return path;
}

const ModelMaterialData& MaterialNamed(const LoadedModelData& model, const std::string& name)
{
    for (const ModelMaterialData& material : model.materials)
    {
        if (material.name == name)
        {
            return material;
        }
    }
    throw std::runtime_error("material '" + name + "' was not loaded");
}

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

void Near(float actual, float expected, const std::string& what)
{
    Require(std::fabs(actual - expected) < 1e-6f, what + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}

void ReadsClearcoat()
{
    const ScopedFixtureDirectory directory;
    const LoadedModelData model = ModelLoader::LoadModel(
        WriteModel(
            directory.path,
            "clearcoat",
            {R"({ "name": "coated", "extensions": { "KHR_materials_clearcoat": { "clearcoatFactor": 0.8, "clearcoatRoughnessFactor": 0.2 } } })",
             R"({ "name": "clamped", "extensions": { "KHR_materials_clearcoat": { "clearcoatFactor": 3, "clearcoatRoughnessFactor": -1 } } })",
             R"({ "name": "defaults", "extensions": { "KHR_materials_clearcoat": {} } })",
             R"({ "name": "plain" })",
             R"({ "name": "textured", "extensions": { "KHR_materials_clearcoat": { "clearcoatFactor": 0.5, "clearcoatTexture": { "index": 0 } } } })"})
            .string());
    Require(model.IsValid(), "the clearcoat fixture loads");

    Near(MaterialNamed(model, "coated").clearcoatFactor, 0.8f, "coated factor");
    Near(MaterialNamed(model, "coated").clearcoatRoughnessFactor, 0.2f, "coated roughness");
    Near(MaterialNamed(model, "coated").pbr.clearcoatFactor, 0.8f, "coated factor in the PBR settings");
    Near(MaterialNamed(model, "clamped").clearcoatFactor, 1.0f, "factor clamped to 1");
    Near(MaterialNamed(model, "clamped").clearcoatRoughnessFactor, 0.0f, "roughness clamped to 0");
    // The extension's own defaults: an empty extension object is a coat of zero.
    Near(MaterialNamed(model, "defaults").clearcoatFactor, 0.0f, "extension default factor");
    Near(MaterialNamed(model, "plain").clearcoatFactor, 0.0f, "no extension, no coat");
    Near(MaterialNamed(model, "plain").clearcoatRoughnessFactor, 0.0f, "no extension, roughness 0");
    // Coat textures are not supported; the factors still apply.
    Near(MaterialNamed(model, "textured").clearcoatFactor, 0.5f, "textured coat keeps its factor");
}

void ReadsSheen()
{
    const ScopedFixtureDirectory directory;
    const LoadedModelData model = ModelLoader::LoadModel(
        WriteModel(
            directory.path,
            "sheen",
            {R"({ "name": "velvet", "extensions": { "KHR_materials_sheen": { "sheenColorFactor": [0.9, 0.5, 0.25], "sheenRoughnessFactor": 0.6 } } })",
             R"({ "name": "clamped", "extensions": { "KHR_materials_sheen": { "sheenColorFactor": [2, -1, 0.5], "sheenRoughnessFactor": 3 } } })",
             R"({ "name": "defaults", "extensions": { "KHR_materials_sheen": {} } })",
             R"({ "name": "plain" })",
             R"({ "name": "textured", "extensions": { "KHR_materials_sheen": { "sheenColorFactor": [1, 1, 1], "sheenColorTexture": { "index": 0 } } } })",
             R"({ "name": "both", "extensions": { "KHR_materials_sheen": { "sheenColorFactor": [1, 1, 1] }, "KHR_materials_clearcoat": { "clearcoatFactor": 1 } } })"})
            .string());
    Require(model.IsValid(), "the sheen fixture loads");

    const ModelMaterialData& velvet = MaterialNamed(model, "velvet");
    Near(velvet.sheenColorFactor[0], 0.9f, "sheen red");
    Near(velvet.sheenColorFactor[1], 0.5f, "sheen green");
    Near(velvet.sheenColorFactor[2], 0.25f, "sheen blue");
    Near(velvet.sheenRoughnessFactor, 0.6f, "sheen roughness");
    Near(velvet.pbr.sheenColorFactor[0], 0.9f, "sheen red in the PBR settings");
    Near(velvet.pbr.sheenRoughnessFactor, 0.6f, "sheen roughness in the PBR settings");

    const ModelMaterialData& clamped = MaterialNamed(model, "clamped");
    Near(clamped.sheenColorFactor[0], 1.0f, "sheen colour clamps to 1");
    Near(clamped.sheenColorFactor[1], 0.0f, "sheen colour clamps to 0");
    Near(clamped.sheenRoughnessFactor, 1.0f, "sheen roughness clamps to 1");

    Near(MaterialNamed(model, "defaults").sheenColorFactor[0], 0.0f, "the extension's default colour is black");
    Near(MaterialNamed(model, "plain").sheenColorFactor[0], 0.0f, "no extension, no sheen");
    Near(MaterialNamed(model, "textured").sheenColorFactor[0], 1.0f, "textured sheen keeps its factors");
    // Both are kept; the renderer decides which layer the pixel gets.
    Near(MaterialNamed(model, "both").sheenColorFactor[0], 1.0f, "sheen kept beside a coat");
    Near(MaterialNamed(model, "both").clearcoatFactor, 1.0f, "coat kept beside a sheen");
}

void SidecarKeepsSheen()
{
    const ScopedFixtureDirectory directory;
    ModelImportedMaterialInfo written{};
    written.name = "velvet";
    written.pbr.sheenColorFactor[0] = 0.5f;
    written.pbr.sheenColorFactor[1] = 0.25f;
    written.pbr.sheenColorFactor[2] = 0.125f;
    written.pbr.sheenRoughnessFactor = 0.75f;
    YAML::Node root;
    root["material"] = SerializeMaterialDefinition(written);
    const std::filesystem::path path = directory.path / "velvet.material.yaml";
    std::ofstream(path) << YAML::Dump(root);

    ModelImportedMaterialInfo read{};
    std::string warning;
    Require(LoadMaterialDefinition(path, read, warning), "the sidecar loads: " + warning);
    Near(read.pbr.sheenColorFactor[1], 0.25f, "sidecar sheen green");
    Near(read.pbr.sheenRoughnessFactor, 0.75f, "sidecar sheen roughness");

    const std::filesystem::path legacyPath = directory.path / "legacy.material.yaml";
    std::ofstream(legacyPath) << "material:\n  name: legacy\n  pbr:\n    roughness_factor: 0.5\n";
    ModelImportedMaterialInfo legacy{};
    Require(LoadMaterialDefinition(legacyPath, legacy, warning), "the legacy sidecar loads: " + warning);
    Near(legacy.pbr.sheenColorFactor[0], 0.0f, "a legacy sidecar has no sheen");

    ModelMaterialData applied{};
    ApplyImportedMaterialInfo(read, applied);
    Near(applied.sheenColorFactor[2], 0.125f, "applied sheen blue");
    Near(applied.sheenRoughnessFactor, 0.75f, "applied sheen roughness");
}

void SidecarKeepsClearcoat()
{
    const ScopedFixtureDirectory directory;
    ModelImportedMaterialInfo written{};
    written.name = "coated";
    written.pbr.clearcoatFactor = 0.75f;
    written.pbr.clearcoatRoughnessFactor = 0.125f;
    YAML::Node root;
    root["material"] = SerializeMaterialDefinition(written);
    const std::filesystem::path path = directory.path / "coated.material.yaml";
    std::ofstream(path) << YAML::Dump(root);

    ModelImportedMaterialInfo read{};
    std::string warning;
    Require(LoadMaterialDefinition(path, read, warning), "the sidecar loads: " + warning);
    Near(read.pbr.clearcoatFactor, 0.75f, "sidecar factor");
    Near(read.pbr.clearcoatRoughnessFactor, 0.125f, "sidecar roughness");

    // A sidecar written before clearcoat existed has neither key.
    const std::filesystem::path legacyPath = directory.path / "legacy.material.yaml";
    std::ofstream(legacyPath) << "material:\n  name: legacy\n  pbr:\n    roughness_factor: 0.5\n";
    ModelImportedMaterialInfo legacy{};
    legacy.pbr.clearcoatFactor = 0.9f;
    Require(LoadMaterialDefinition(legacyPath, legacy, warning), "the legacy sidecar loads: " + warning);
    Near(legacy.pbr.clearcoatFactor, 0.9f, "a missing key leaves the factor as it was");

    // Out-of-range values in a hand-edited sidecar clamp.
    const std::filesystem::path wildPath = directory.path / "wild.material.yaml";
    std::ofstream(wildPath) << "material:\n  name: wild\n  pbr:\n    clearcoat_factor: 2\n    clearcoat_roughness_factor: -3\n";
    ModelImportedMaterialInfo wild{};
    Require(LoadMaterialDefinition(wildPath, wild, warning), "the wild sidecar loads: " + warning);
    Near(wild.pbr.clearcoatFactor, 1.0f, "sidecar factor clamps");
    Near(wild.pbr.clearcoatRoughnessFactor, 0.0f, "sidecar roughness clamps");

    ModelMaterialData applied{};
    ApplyImportedMaterialInfo(read, applied);
    Near(applied.clearcoatFactor, 0.75f, "applied factor");
    Near(applied.clearcoatRoughnessFactor, 0.125f, "applied roughness");
}
}

int main()
{
    try
    {
        ReadsEmissiveStrength();
        ReadsClearcoat();
        SidecarKeepsClearcoat();
        ReadsSheen();
        SidecarKeepsSheen();
    }
    catch (const std::exception& error)
    {
        std::cerr << "glTF material extension tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "glTF material extension tests passed\n";
    return 0;
}
