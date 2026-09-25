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
std::filesystem::path WriteModel(
    const std::filesystem::path& directory,
    const std::string& name,
    const std::vector<std::string>& materials,
    const std::string& extraRootMembers = "")
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
    file << R"({ "asset": { "version": "2.0" }, )" << extraRootMembers << R"( "materials": [)" << materialList << R"(],
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

// Five images, one per layer map, each its own texture so a mix-up between them shows.
const char* kLayerTextures = R"("images": [{ "uri": "cc.png" }, { "uri": "ccr.png" }, { "uri": "sc.png" }, { "uri": "sr.png" }, { "uri": "an.png" },
                 { "uri": "sp.png" }, { "uri": "spc.png" }, { "uri": "ccn.png" }],
  "textures": [{ "source": 0 }, { "source": 1 }, { "source": 2 }, { "source": 3 }, { "source": 4 },
               { "source": 5 }, { "source": 6 }, { "source": 7 }],)";

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
             R"({ "name": "textured", "extensions": { "KHR_materials_clearcoat": { "clearcoatFactor": 0.5, "clearcoatTexture": { "index": 0 } } } })"},
            kLayerTextures)
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
    // A coat texture multiplies the factor, which is kept as it is.
    Near(MaterialNamed(model, "textured").clearcoatFactor, 0.5f, "textured coat keeps its factor");
    Require(MaterialNamed(model, "textured").clearcoatTexturePath == "cc.png", "textured coat has its map");
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
             R"({ "name": "both", "extensions": { "KHR_materials_sheen": { "sheenColorFactor": [1, 1, 1] }, "KHR_materials_clearcoat": { "clearcoatFactor": 1 } } })"},
            kLayerTextures)
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

void ReadsLayerTextures()
{
    const ScopedFixtureDirectory directory;
    const LoadedModelData model = ModelLoader::LoadModel(
        WriteModel(
            directory.path,
            "layers",
            {R"({ "name": "layers", "extensions": {
                   "KHR_materials_clearcoat": { "clearcoatFactor": 1, "clearcoatTexture": { "index": 0 }, "clearcoatRoughnessTexture": { "index": 1 },
                                                "clearcoatNormalTexture": { "index": 7 } },
                   "KHR_materials_sheen": { "sheenColorFactor": [1, 1, 1], "sheenColorTexture": { "index": 2 }, "sheenRoughnessTexture": { "index": 3 } },
                   "KHR_materials_anisotropy": { "anisotropyStrength": 0.5, "anisotropyTexture": { "index": 4 } } } })",
             R"({ "name": "plain" })"},
            kLayerTextures)
            .string());
    Require(model.IsValid(), "the layer texture fixture loads");
    const ModelMaterialData& layers = MaterialNamed(model, "layers");
    Require(layers.clearcoatTexturePath == "cc.png", "clearcoat texture: " + layers.clearcoatTexturePath);
    Require(layers.clearcoatRoughnessTexturePath == "ccr.png", "clearcoat roughness texture: " + layers.clearcoatRoughnessTexturePath);
    Require(layers.sheenColorTexturePath == "sc.png", "sheen colour texture: " + layers.sheenColorTexturePath);
    Require(layers.sheenRoughnessTexturePath == "sr.png", "sheen roughness texture: " + layers.sheenRoughnessTexturePath);
    Require(layers.anisotropyTexturePath == "an.png", "anisotropy texture: " + layers.anisotropyTexturePath);
    const ModelMaterialData& plain = MaterialNamed(model, "plain");
    Require(plain.clearcoatTexturePath.empty() && plain.sheenColorTexturePath.empty() && plain.anisotropyTexturePath.empty(),
            "a material without the extensions has no layer maps");
}

void ReadsAnisotropy()
{
    const ScopedFixtureDirectory directory;
    const LoadedModelData model = ModelLoader::LoadModel(
        WriteModel(
            directory.path,
            "anisotropy",
            {R"({ "name": "brushed", "extensions": { "KHR_materials_anisotropy": { "anisotropyStrength": 0.6, "anisotropyRotation": 1.25 } } })",
             R"({ "name": "clamped", "extensions": { "KHR_materials_anisotropy": { "anisotropyStrength": 4, "anisotropyRotation": -2 } } })",
             R"({ "name": "defaults", "extensions": { "KHR_materials_anisotropy": {} } })",
             R"({ "name": "plain" })"})
            .string());
    Require(model.IsValid(), "the anisotropy fixture loads");
    Near(MaterialNamed(model, "brushed").anisotropyStrength, 0.6f, "strength");
    Near(MaterialNamed(model, "brushed").anisotropyRotation, 1.25f, "rotation");
    Near(MaterialNamed(model, "brushed").pbr.anisotropyStrength, 0.6f, "strength in the PBR settings");
    Near(MaterialNamed(model, "brushed").pbr.anisotropyRotation, 1.25f, "rotation in the PBR settings");
    Near(MaterialNamed(model, "clamped").anisotropyStrength, 1.0f, "strength clamps to 1");
    Near(MaterialNamed(model, "clamped").anisotropyRotation, -2.0f, "any rotation is kept");
    Near(MaterialNamed(model, "defaults").anisotropyStrength, 0.0f, "the extension's default strength is 0");
    Near(MaterialNamed(model, "plain").anisotropyStrength, 0.0f, "no extension, isotropic");
}

void SidecarKeepsAnisotropyAndLayerMaps()
{
    const ScopedFixtureDirectory directory;
    ModelImportedMaterialInfo written{};
    written.name = "brushed";
    written.pbr.anisotropyStrength = 0.5f;
    written.pbr.anisotropyRotation = 0.75f;
    written.clearcoatTexturePath = "cc.png";
    written.clearcoatRoughnessTexturePath = "ccr.png";
    written.sheenColorTexturePath = "sc.png";
    written.sheenRoughnessTexturePath = "sr.png";
    written.anisotropyTexturePath = "an.png";
    YAML::Node root;
    root["material"] = SerializeMaterialDefinition(written);
    const std::filesystem::path path = directory.path / "brushed.material.yaml";
    std::ofstream(path) << YAML::Dump(root);

    ModelImportedMaterialInfo read{};
    std::string warning;
    Require(LoadMaterialDefinition(path, read, warning), "the sidecar loads: " + warning);
    Near(read.pbr.anisotropyStrength, 0.5f, "sidecar strength");
    Near(read.pbr.anisotropyRotation, 0.75f, "sidecar rotation");
    Require(read.clearcoatTexturePath == "cc.png" && read.clearcoatRoughnessTexturePath == "ccr.png" &&
                read.sheenColorTexturePath == "sc.png" && read.sheenRoughnessTexturePath == "sr.png" &&
                read.anisotropyTexturePath == "an.png",
            "sidecar layer maps");

    const std::filesystem::path legacyPath = directory.path / "legacy.material.yaml";
    std::ofstream(legacyPath) << "material:\n  name: legacy\n  pbr:\n    roughness_factor: 0.5\n";
    ModelImportedMaterialInfo legacy{};
    Require(LoadMaterialDefinition(legacyPath, legacy, warning), "the legacy sidecar loads: " + warning);
    Near(legacy.pbr.anisotropyStrength, 0.0f, "a legacy sidecar is isotropic");
    Require(legacy.anisotropyTexturePath.empty(), "a legacy sidecar has no layer maps");

    ModelMaterialData applied{};
    ApplyImportedMaterialInfo(read, applied);
    Near(applied.anisotropyStrength, 0.5f, "applied strength");
    Near(applied.anisotropyRotation, 0.75f, "applied rotation");
    Require(applied.sheenRoughnessTexturePath == "sr.png", "applied layer maps");
}

void ReadsIorAndSpecular()
{
    const ScopedFixtureDirectory directory;
    const LoadedModelData model = ModelLoader::LoadModel(
        WriteModel(
            directory.path,
            "specular",
            {R"({ "name": "diamond", "extensions": { "KHR_materials_ior": { "ior": 2.4 },
                   "KHR_materials_specular": { "specularFactor": 0.5, "specularColorFactor": [1, 0.5, 2],
                                               "specularTexture": { "index": 5 }, "specularColorTexture": { "index": 6 } } } })",
             R"({ "name": "infinite", "extensions": { "KHR_materials_ior": { "ior": 0 } } })",
             R"({ "name": "invalid", "extensions": { "KHR_materials_ior": { "ior": 0.5 }, "KHR_materials_specular": { "specularFactor": 3 } } })",
             R"({ "name": "coatnormal", "extensions": { "KHR_materials_clearcoat": { "clearcoatFactor": 1,
                   "clearcoatNormalTexture": { "index": 7, "scale": 0.25 } } } })",
             R"({ "name": "plain" })"},
            kLayerTextures)
            .string());
    Require(model.IsValid(), "the specular fixture loads");
    const ModelMaterialData& diamond = MaterialNamed(model, "diamond");
    Near(diamond.ior, 2.4f, "ior");
    Near(diamond.specularFactor, 0.5f, "specular factor");
    Near(diamond.specularColorFactor[2], 2.0f, "a specular colour above 1 is kept");
    Near(diamond.pbr.ior, 2.4f, "ior in the PBR settings");
    Near(diamond.pbr.specularColorFactor[1], 0.5f, "specular colour in the PBR settings");
    Require(diamond.specularTexturePath == "sp.png" && diamond.specularColorTexturePath == "spc.png", "specular maps");
    Near(MaterialNamed(model, "infinite").ior, 0.0f, "0 is an infinite index");
    Near(MaterialNamed(model, "invalid").ior, 1.5f, "an index below 1 reads as the default");
    Near(MaterialNamed(model, "invalid").specularFactor, 1.0f, "the specular factor clamps to 1");
    const ModelMaterialData& coat = MaterialNamed(model, "coatnormal");
    Require(coat.clearcoatNormalTexturePath == "ccn.png", "coat normal map");
    Near(coat.clearcoatNormalScale, 0.25f, "coat normal scale");
    const ModelMaterialData& plain = MaterialNamed(model, "plain");
    Near(plain.ior, 1.5f, "default ior");
    Near(plain.specularFactor, 1.0f, "default specular");
    Near(plain.specularColorFactor[0], 1.0f, "default specular colour");
    Near(plain.clearcoatNormalScale, 1.0f, "default coat normal scale");
}

void DielectricF0FollowsKhronos()
{
    const float white[3] = {1.0f, 1.0f, 1.0f};
    float f0[3];
    ComputeDielectricF0(1.5f, white, 1.0f, f0);
    Near(f0[0], 0.04f, "ior 1.5 is F0 0.04");
    ComputeDielectricF0(1.0f, white, 1.0f, f0);
    Near(f0[0], 0.0f, "ior 1 reflects nothing");
    ComputeDielectricF0(0.0f, white, 1.0f, f0);
    Near(f0[0], 1.0f, "an infinite index reflects everything");
    const float tint[3] = {1.0f, 0.5f, 100.0f};
    ComputeDielectricF0(1.5f, tint, 0.5f, f0);
    Near(f0[1], 0.01f, "the colour tints and the factor scales");
    Near(f0[2], 0.5f, "F0 is clamped to 1 before the factor");
    Near(SanitizeIor(-2.0f), 1.5f, "a negative index is invalid");
}

void SidecarKeepsIorAndSpecular()
{
    const ScopedFixtureDirectory directory;
    ModelImportedMaterialInfo written{};
    written.name = "gem";
    written.pbr.ior = 2.0f;
    written.pbr.specularFactor = 0.25f;
    written.pbr.specularColorFactor[1] = 0.5f;
    written.pbr.clearcoatNormalScale = 0.5f;
    written.specularTexturePath = "sp.png";
    written.specularColorTexturePath = "spc.png";
    written.clearcoatNormalTexturePath = "ccn.png";
    YAML::Node root;
    root["material"] = SerializeMaterialDefinition(written);
    const std::filesystem::path path = directory.path / "gem.material.yaml";
    std::ofstream(path) << YAML::Dump(root);

    ModelImportedMaterialInfo read{};
    std::string warning;
    Require(LoadMaterialDefinition(path, read, warning), "the sidecar loads: " + warning);
    Near(read.pbr.ior, 2.0f, "sidecar ior");
    Near(read.pbr.specularFactor, 0.25f, "sidecar specular");
    Near(read.pbr.specularColorFactor[1], 0.5f, "sidecar specular colour");
    Near(read.pbr.clearcoatNormalScale, 0.5f, "sidecar coat normal scale");
    Require(read.specularTexturePath == "sp.png" && read.specularColorTexturePath == "spc.png" &&
                read.clearcoatNormalTexturePath == "ccn.png",
            "sidecar maps");

    const std::filesystem::path legacyPath = directory.path / "legacy.material.yaml";
    std::ofstream(legacyPath) << "material:\n  name: legacy\n  pbr:\n    roughness_factor: 0.5\n";
    ModelImportedMaterialInfo legacy{};
    Require(LoadMaterialDefinition(legacyPath, legacy, warning), "the legacy sidecar loads: " + warning);
    Near(legacy.pbr.ior, 1.5f, "a legacy sidecar has the default ior");
    Near(legacy.pbr.specularFactor, 1.0f, "and the default specular");

    ModelMaterialData applied{};
    ApplyImportedMaterialInfo(read, applied);
    Near(applied.ior, 2.0f, "applied ior");
    Near(applied.specularColorFactor[1], 0.5f, "applied specular colour");
    Require(applied.clearcoatNormalTexturePath == "ccn.png", "applied coat normal map");
}

void ReadsIridescence()
{
    const ScopedFixtureDirectory directory;
    const LoadedModelData model = ModelLoader::LoadModel(
        WriteModel(
            directory.path,
            "iridescence",
            {R"({ "name": "bubble", "extensions": { "KHR_materials_iridescence": { "iridescenceFactor": 0.75, "iridescenceIor": 1.4,
                   "iridescenceThicknessMinimum": 250, "iridescenceThicknessMaximum": 600,
                   "iridescenceTexture": { "index": 0 }, "iridescenceThicknessTexture": { "index": 1 } } } })",
             R"({ "name": "defaults", "extensions": { "KHR_materials_iridescence": {} } })",
             R"({ "name": "wild", "extensions": { "KHR_materials_iridescence": { "iridescenceFactor": 5, "iridescenceIor": 0.2,
                   "iridescenceThicknessMinimum": -3 } } })"},
            kLayerTextures)
            .string());
    Require(model.IsValid(), "the iridescence fixture loads");
    const ModelMaterialData& bubble = MaterialNamed(model, "bubble");
    Near(bubble.iridescenceFactor, 0.75f, "factor");
    Near(bubble.iridescenceIor, 1.4f, "film ior");
    Near(bubble.iridescenceThicknessMinimum, 250.0f, "thickness minimum");
    Near(bubble.iridescenceThicknessMaximum, 600.0f, "thickness maximum");
    Near(bubble.pbr.iridescenceFactor, 0.75f, "factor in the PBR settings");
    Require(bubble.iridescenceTexturePath == "cc.png" && bubble.iridescenceThicknessTexturePath == "ccr.png", "iridescence maps");
    const ModelMaterialData& defaults = MaterialNamed(model, "defaults");
    Near(defaults.iridescenceFactor, 0.0f, "default factor");
    Near(defaults.iridescenceIor, 1.3f, "default film ior");
    Near(defaults.iridescenceThicknessMinimum, 100.0f, "default minimum");
    Near(defaults.iridescenceThicknessMaximum, 400.0f, "default maximum");
    const ModelMaterialData& wild = MaterialNamed(model, "wild");
    Near(wild.iridescenceFactor, 1.0f, "factor clamps to 1");
    Near(wild.iridescenceIor, 1.0f, "film ior clamps to 1");
    Near(wild.iridescenceThicknessMinimum, 0.0f, "thickness clamps to 0");

    ModelImportedMaterialInfo info = BuildImportedMaterialInfo(bubble);
    YAML::Node root;
    root["material"] = SerializeMaterialDefinition(info);
    const std::filesystem::path path = directory.path / "bubble.material.yaml";
    std::ofstream(path) << YAML::Dump(root);
    ModelImportedMaterialInfo read{};
    std::string warning;
    Require(LoadMaterialDefinition(path, read, warning), "the sidecar loads: " + warning);
    Near(read.pbr.iridescenceIor, 1.4f, "sidecar film ior");
    Near(read.pbr.iridescenceThicknessMaximum, 600.0f, "sidecar maximum");
    Require(read.iridescenceThicknessTexturePath == "ccr.png", "sidecar thickness map");
}

void ReadsTextureTransformsAndUnlit()
{
    const ScopedFixtureDirectory directory;
    const LoadedModelData model = ModelLoader::LoadModel(
        WriteModel(
            directory.path,
            "transforms",
            {R"({ "name": "transformed",
                 "pbrMetallicRoughness": {
                   "baseColorTexture": { "index": 0, "texCoord": 1 },
                   "metallicRoughnessTexture": { "index": 1, "extensions": { "KHR_texture_transform": {
                     "offset": [0.5, 0.25], "rotation": 1.5, "scale": [2, 3] } } } },
                 "normalTexture": { "index": 2, "extensions": { "KHR_texture_transform": { "scale": [4, 4], "texCoord": 1 } } },
                 "extensions": { "KHR_materials_clearcoat": { "clearcoatFactor": 1,
                   "clearcoatTexture": { "index": 0, "texCoord": 1, "extensions": { "KHR_texture_transform": { "offset": [0.1, 0.2] } } } } } })",
             R"({ "name": "flat", "extensions": { "KHR_materials_unlit": {} } })",
             R"({ "name": "plain" })"},
            kLayerTextures)
            .string());
    Require(model.IsValid(), "the transform fixture loads");
    const ModelMaterialData& transformed = MaterialNamed(model, "transformed");
    const auto transformOf = [&](MaterialTextureSlot slot)
    {
        return transformed.textureTransforms[static_cast<size_t>(slot)];
    };
    Require(transformOf(MaterialTextureSlot::BaseColor).texCoord == 1, "texCoord 1 on the base colour");
    Require(transformOf(MaterialTextureSlot::BaseColor).scale[0] == 1.0f, "no transform, identity otherwise");
    Near(transformOf(MaterialTextureSlot::Metallic).offset[0], 0.5f, "metallic offset");
    Near(transformOf(MaterialTextureSlot::Roughness).rotation, 1.5f, "roughness shares the metallic-roughness transform");
    Near(transformOf(MaterialTextureSlot::Roughness).scale[1], 3.0f, "roughness scale");
    Require(transformOf(MaterialTextureSlot::Normal).texCoord == 1, "the extension's texCoord overrides");
    Near(transformOf(MaterialTextureSlot::Normal).scale[0], 4.0f, "normal scale");
    Require(transformOf(MaterialTextureSlot::Clearcoat).texCoord == 1, "an extension texture's texCoord");
    Near(transformOf(MaterialTextureSlot::Clearcoat).offset[1], 0.2f, "an extension texture's transform");
    Require(transformOf(MaterialTextureSlot::Emissive).IsIdentity(), "untouched slots stay identity");
    Require(!transformed.unlit, "a lit material");
    Require(MaterialNamed(model, "flat").unlit && MaterialNamed(model, "flat").pbr.unlit, "KHR_materials_unlit");
    Require(AreIdentity(MaterialNamed(model, "plain").textureTransforms), "a plain material has no transforms");

    ModelImportedMaterialInfo info = BuildImportedMaterialInfo(transformed);
    info.pbr.unlit = true;
    YAML::Node root;
    root["material"] = SerializeMaterialDefinition(info);
    const std::filesystem::path path = directory.path / "transformed.material.yaml";
    std::ofstream(path) << YAML::Dump(root);
    ModelImportedMaterialInfo read{};
    std::string warning;
    Require(LoadMaterialDefinition(path, read, warning), "the sidecar loads: " + warning);
    Require(read.pbr.unlit, "sidecar unlit");
    Near(read.textureTransforms[static_cast<size_t>(MaterialTextureSlot::Roughness)].rotation, 1.5f, "sidecar rotation");
    Near(read.textureTransforms[static_cast<size_t>(MaterialTextureSlot::Metallic)].scale[0], 2.0f, "sidecar scale");
    Require(read.textureTransforms[static_cast<size_t>(MaterialTextureSlot::Normal)].texCoord == 1, "sidecar texCoord");
    Require(read.textureTransforms[static_cast<size_t>(MaterialTextureSlot::Emissive)].IsIdentity(), "sidecar identity slots");

    const std::filesystem::path legacyPath = directory.path / "legacy.material.yaml";
    std::ofstream(legacyPath) << "material:\n  name: legacy\n  pbr:\n    roughness_factor: 0.5\n";
    ModelImportedMaterialInfo legacy{};
    Require(LoadMaterialDefinition(legacyPath, legacy, warning), "the legacy sidecar loads: " + warning);
    Require(!legacy.pbr.unlit && AreIdentity(legacy.textureTransforms), "a legacy sidecar is lit and untransformed");

    ModelMaterialData applied{};
    ApplyImportedMaterialInfo(read, applied);
    Require(applied.unlit, "applied unlit");
    Near(applied.textureTransforms[static_cast<size_t>(MaterialTextureSlot::Clearcoat)].offset[0], 0.1f, "applied transform");
}

void TransformRowsFollowGltf()
{
    float row0[4];
    float row1[4];
    ComputeTextureTransformRows(TextureTransform{}, row0, row1);
    Require(row0[0] == 1.0f && row0[1] == 0.0f && row0[2] == 0.0f && row1[0] == 0.0f && row1[1] == 1.0f && row1[2] == 0.0f,
            "identity");
    const auto apply = [&](float u, float v)
    {
        return std::array<float, 2>{row0[0] * u + row0[1] * v + row0[2], row1[0] * u + row1[1] * v + row1[2]};
    };
    TextureTransform offset{};
    offset.offset[0] = 0.5f;
    offset.offset[1] = -0.25f;
    ComputeTextureTransformRows(offset, row0, row1);
    Near(apply(0.1f, 0.2f)[0], 0.6f, "offset u");
    Near(apply(0.1f, 0.2f)[1], -0.05f, "offset v");
    TextureTransform rotation{};
    rotation.rotation = 1.57079633f;
    ComputeTextureTransformRows(rotation, row0, row1);
    // glTF's R = [[cos, sin], [-sin, cos]]: (1, 0) goes to (0, -1).
    Require(std::fabs(apply(1.0f, 0.0f)[0]) < 1e-6f && std::fabs(apply(1.0f, 0.0f)[1] + 1.0f) < 1e-6f, "a quarter turn");
    TextureTransform all{};
    all.offset[0] = 1.0f;
    all.rotation = 1.57079633f;
    all.scale[0] = 2.0f;
    all.scale[1] = 3.0f;
    all.texCoord = 1;
    ComputeTextureTransformRows(all, row0, row1);
    // T R S (0, 1): scale to (0, 3), rotate to (3, 0), offset to (4, 0).
    Require(std::fabs(apply(0.0f, 1.0f)[0] - 4.0f) < 1e-5f && std::fabs(apply(0.0f, 1.0f)[1]) < 1e-5f, "scale, then rotate, then offset");
    Require(row0[3] == 1.0f, "the UV set rides in row0.w");
}

void ReadsSecondUvSet()
{
    const ScopedFixtureDirectory directory;
    const std::filesystem::path path = directory.path / "uv1.gltf";
    std::ofstream file(path);
    file << R"({ "asset": { "version": "2.0" },
      "buffers": [{ "uri": "uv1.bin", "byteLength": 66 }],
      "bufferViews": [
        { "buffer": 0, "byteOffset": 0, "byteLength": 36 },
        { "buffer": 0, "byteOffset": 36, "byteLength": 24 },
        { "buffer": 0, "byteOffset": 60, "byteLength": 6 }
      ],
      "accessors": [
        { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
        { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2" },
        { "bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR" }
      ],
      "meshes": [{ "primitives": [{ "attributes": { "POSITION": 0, "TEXCOORD_0": 1, "TEXCOORD_1": 1 }, "indices": 2 }] }],
      "nodes": [{ "mesh": 0 }], "scenes": [{ "nodes": [0] }], "scene": 0 })";
    file.close();
    std::ofstream buffer(directory.path / "uv1.bin", std::ios::binary);
    const std::array<float, 9> positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const std::array<float, 6> uvs = {0.0f, 0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    const std::array<uint16_t, 3> indices = {0, 1, 2};
    buffer.write(reinterpret_cast<const char*>(positions.data()), sizeof(positions));
    buffer.write(reinterpret_cast<const char*>(uvs.data()), sizeof(uvs));
    buffer.write(reinterpret_cast<const char*>(indices.data()), sizeof(indices));
    buffer.close();
    const LoadedModelData model = ModelLoader::LoadModel(path.string());
    Require(model.IsValid(), "the second UV set fixture loads");
    bool found = false;
    for (const Vertex& vertex : model.submeshes[0].mesh.vertices)
    {
        found = found || (vertex.texCoord1[0] == 0.25f && vertex.texCoord1[1] == 0.5f);
    }
    Require(found, "TEXCOORD_1 reaches the vertices");
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
        ReadsLayerTextures();
        ReadsAnisotropy();
        SidecarKeepsAnisotropyAndLayerMaps();
        ReadsIorAndSpecular();
        DielectricF0FollowsKhronos();
        SidecarKeepsIorAndSpecular();
        ReadsIridescence();
        ReadsTextureTransformsAndUnlit();
        TransformRowsFollowGltf();
        ReadsSecondUvSet();
    }
    catch (const std::exception& error)
    {
        std::cerr << "glTF material extension tests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "glTF material extension tests passed\n";
    return 0;
}
