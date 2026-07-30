#include <material_graph.h>
#include <material_definition.h>
#include <model_loader.h>

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

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool NearlyEqual(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 0.0001f;
}

std::filesystem::path CreateFixtureDirectory()
{
    const std::filesystem::path temporaryDirectory = std::filesystem::temp_directory_path();
    std::random_device randomDevice;
    for (size_t attempt = 0; attempt < 100; ++attempt)
    {
        const std::filesystem::path path = temporaryDirectory / (
            "miniengine_material_alpha_" +
            std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()) + "_" +
            std::to_string(randomDevice())
        );
        std::error_code error;
        if (std::filesystem::create_directory(path, error))
        {
            return path;
        }
        if (error && error != std::errc::file_exists)
        {
            throw std::runtime_error("Failed to create material alpha fixture directory: " + error.message());
        }
    }

    throw std::runtime_error("Failed to create a unique material alpha fixture directory");
}

class ScopedFixtureDirectory
{
public:
    ScopedFixtureDirectory()
        : path(CreateFixtureDirectory())
    {
    }

    ~ScopedFixtureDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    const std::filesystem::path path;
};

std::filesystem::path WriteAlphaModeFixture(const std::filesystem::path& fixtureDirectory)
{
    const std::filesystem::path path = fixtureDirectory / "miniengine_material_alpha_modes.gltf";
    std::ofstream file(path);
    file << R"({
      "asset": { "version": "2.0" },
      "materials": [
        { "name": "opaque", "pbrMetallicRoughness": { "baseColorFactor": [1,1,1,0.25] } },
        { "name": "mask", "alphaMode": "MASK", "alphaCutoff": 0.4,
          "pbrMetallicRoughness": { "baseColorFactor": [1,1,1,0.5] } },
        { "name": "blend", "alphaMode": "BLEND",
          "pbrMetallicRoughness": { "baseColorFactor": [1,1,1,0.75] } }
      ],
      "buffers": [{ "uri": "miniengine_material_alpha_modes.bin", "byteLength": 42 }],
      "bufferViews": [
        { "buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962 },
        { "buffer": 0, "byteOffset": 36, "byteLength": 6, "target": 34963 }
      ],
      "accessors": [
        { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3" },
        { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }
      ],
      "meshes": [{ "primitives": [{ "attributes": { "POSITION": 0 }, "indices": 1, "material": 0 }]}],
      "nodes": [{ "mesh": 0 }],
      "scenes": [{ "nodes": [0] }],
      "scene": 0
    })";
    file.close();

    const std::filesystem::path bufferPath = fixtureDirectory / "miniengine_material_alpha_modes.bin";
    std::ofstream buffer(bufferPath, std::ios::binary);
    const std::array<float, 9> positions = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    const std::array<uint16_t, 3> indices = { 0, 1, 2 };
    buffer.write(reinterpret_cast<const char*>(positions.data()), static_cast<std::streamsize>(sizeof(positions)));
    buffer.write(reinterpret_cast<const char*>(indices.data()), static_cast<std::streamsize>(sizeof(indices)));
    return path;
}
}

int main()
{
    try
    {
        Require(ParseMaterialAlphaMode("OPAQUE") == MaterialAlphaMode::Opaque, "OPAQUE parse failed");
        Require(ParseMaterialAlphaMode("mask") == MaterialAlphaMode::Mask, "mask parse failed");
        Require(ParseMaterialAlphaMode("Blend") == MaterialAlphaMode::Blend, "Blend parse failed");
        Require(!ParseMaterialAlphaMode("invalid").has_value(), "invalid alpha mode was accepted");
        Require(std::string(ToString(MaterialAlphaMode::Opaque)) == "opaque", "Opaque string mismatch");
        Require(std::string(ToString(MaterialAlphaMode::Mask)) == "mask", "Mask string mismatch");
        Require(std::string(ToString(MaterialAlphaMode::Blend)) == "blend", "Blend string mismatch");

        Require(NearlyEqual(ClampMaterialAlphaValue(-1.0f), 0.0f), "negative alpha was not clamped");
        Require(NearlyEqual(ClampMaterialAlphaValue(2.0f), 1.0f), "alpha above one was not clamped");
        Require(NearlyEqual(
            ResolveMaterialCoverageAlpha(MaterialAlphaMode::Opaque, 0.2f, 0.5f),
            1.0f
        ), "Opaque did not force full coverage");
        Require(NearlyEqual(
            ResolveMaterialCoverageAlpha(MaterialAlphaMode::Mask, 0.49f, 0.5f),
            0.0f
        ), "Mask did not discard below cutoff");
        Require(NearlyEqual(
            ResolveMaterialCoverageAlpha(MaterialAlphaMode::Mask, 0.5f, 0.5f),
            1.0f
        ), "Mask incorrectly discarded at cutoff");
        Require(NearlyEqual(
            ResolveMaterialCoverageAlpha(MaterialAlphaMode::Blend, 0.25f, 0.5f),
            0.25f
        ), "Blend did not preserve alpha");

        ModelImportedMaterialInfo source{};
        source.name = "masked foliage";
        source.pbr.alphaMode = MaterialAlphaMode::Mask;
        source.pbr.alphaCutoff = 0.37f;
        source.pbr.baseColorFactor[3] = 0.6f;
        source.pbr.opacity = 0.8f;

        const YAML::Node serialized = SerializeMaterialDefinition(source);
        Require(
            serialized["pbr"]["alpha_mode"].as<std::string>() == "mask",
            "sidecar alpha mode was not canonical"
        );
        Require(
            NearlyEqual(serialized["pbr"]["alpha_cutoff"].as<float>(), 0.37f),
            "sidecar cutoff was not serialized"
        );

        const std::filesystem::path sidecar =
            std::filesystem::temp_directory_path() / "miniengine_material_alpha.material.yaml";
        {
            std::ofstream file(sidecar);
            file << "material:\n"
                    "  name: restored\n"
                    "  pbr:\n"
                    "    alpha_mode: blend\n"
                    "    alpha_cutoff: 2.0\n"
                    "    base_color_factor: [1, 1, 1, 0.4]\n"
                    "    opacity: -1.0\n";
        }
        ModelImportedMaterialInfo restored{};
        std::string warning;
        Require(LoadMaterialDefinition(sidecar, restored, warning), "sidecar did not load");
        Require(restored.pbr.alphaMode == MaterialAlphaMode::Blend, "sidecar mode did not load");
        Require(NearlyEqual(restored.pbr.alphaCutoff, 1.0f), "sidecar cutoff was not clamped");
        Require(NearlyEqual(restored.pbr.opacity, 0.0f), "sidecar opacity was not clamped");

        {
            std::ofstream file(sidecar);
            file << "material:\n  pbr:\n    alpha_mode: invalid\n";
        }
        restored = {};
        warning.clear();
        Require(LoadMaterialDefinition(sidecar, restored, warning), "invalid-mode sidecar did not load");
        Require(restored.pbr.alphaMode == MaterialAlphaMode::Opaque, "invalid mode did not fall back");
        Require(!warning.empty(), "invalid mode did not report a warning");
        std::error_code removeError;
        std::filesystem::remove(sidecar, removeError);

        const ScopedFixtureDirectory reloadFixtureDirectory;
        const std::filesystem::path reloadModel = WriteAlphaModeFixture(reloadFixtureDirectory.path);
        const std::filesystem::path reloadSidecar = BuildMaterialDefinitionPath(reloadModel, 0);
        {
            std::ofstream file(reloadSidecar);
            file << "material:\n  pbr:\n    alpha_mode: mask\n    alpha_cutoff: 0.42\n";
        }
        const LoadedModelData reloaded = ModelLoader::LoadModel(reloadModel.string());
        Require(reloaded.materials[0].pbr.alphaMode == MaterialAlphaMode::Mask, "restart lost alpha mode");
        Require(NearlyEqual(reloaded.materials[0].pbr.alphaCutoff, 0.42f), "restart lost cutoff");
        std::filesystem::remove(reloadSidecar, removeError);

        const ScopedFixtureDirectory legacyFixtureDirectory;
        const std::filesystem::path legacyModel = WriteAlphaModeFixture(legacyFixtureDirectory.path);
        const std::filesystem::path legacySidecar = BuildMaterialDefinitionPath(legacyModel, 1);
        {
            std::ofstream file(legacySidecar);
            file << "material:\n  pbr:\n    metallic_factor: 0.7\n";
        }
        const LoadedModelData legacyReloaded = ModelLoader::LoadModel(legacyModel.string());
        Require(
            legacyReloaded.materials[1].pbr.alphaMode == MaterialAlphaMode::Mask,
            "legacy sidecar lost alpha mode"
        );
        Require(
            NearlyEqual(legacyReloaded.materials[1].pbr.alphaCutoff, 0.4f),
            "legacy sidecar lost alpha cutoff"
        );
        std::filesystem::remove(legacySidecar, removeError);

        const ScopedFixtureDirectory fixtureDirectory;
        const std::filesystem::path fixture = WriteAlphaModeFixture(fixtureDirectory.path);
        const LoadedModelData loaded = ModelLoader::LoadModel(fixture.string());
        Require(loaded.materials.size() == 3, "glTF material count mismatch");
        Require(loaded.materials[0].alphaMode == MaterialAlphaMode::Opaque, "default Opaque was lost");
        Require(loaded.materials[1].alphaMode == MaterialAlphaMode::Mask, "MASK was lost");
        Require(NearlyEqual(loaded.materials[1].alphaCutoff, 0.4f), "MASK cutoff was lost");
        Require(loaded.materials[1].pbr.alphaMode == MaterialAlphaMode::Mask, "MASK PBR mode was lost");
        Require(NearlyEqual(loaded.materials[1].pbr.alphaCutoff, 0.4f), "MASK PBR cutoff was lost");
        Require(NearlyEqual(loaded.materials[1].pbr.opacity, 1.0f), "MASK PBR opacity changed");
        Require(loaded.materials[2].alphaMode == MaterialAlphaMode::Blend, "BLEND was lost");
        Require(NearlyEqual(loaded.materials[2].alphaCutoff, 0.5f), "BLEND cutoff default changed");
        Require(loaded.materials[2].pbr.alphaMode == MaterialAlphaMode::Blend, "BLEND PBR mode was lost");
        Require(NearlyEqual(loaded.materials[2].pbr.alphaCutoff, 0.5f), "BLEND PBR cutoff default changed");
        Require(NearlyEqual(loaded.materials[2].pbr.opacity, 1.0f), "BLEND PBR opacity changed");
        Require(NearlyEqual(loaded.materials[2].baseColor[3], 0.75f), "base alpha changed");
        Require(NearlyEqual(loaded.materials[2].opacity, 1.0f), "imported alpha was duplicated into opacity");

        std::cout << "material alpha tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "material alpha tests failed: " << error.what() << '\n';
        return 1;
    }
}
