#include <material_graph.h>
#include <model_loader.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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

std::filesystem::path WriteAlphaModeFixture()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "miniengine_material_alpha_modes.gltf";
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

    const std::filesystem::path bufferPath = path.parent_path() / "miniengine_material_alpha_modes.bin";
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

        const std::filesystem::path fixture = WriteAlphaModeFixture();
        const LoadedModelData loaded = ModelLoader::LoadModel(fixture.string());
        std::error_code removeError;
        std::filesystem::remove(fixture, removeError);
        std::filesystem::remove(fixture.parent_path() / "miniengine_material_alpha_modes.bin", removeError);
        Require(loaded.materials.size() == 3, "glTF material count mismatch");
        Require(loaded.materials[0].alphaMode == MaterialAlphaMode::Opaque, "default Opaque was lost");
        Require(loaded.materials[1].alphaMode == MaterialAlphaMode::Mask, "MASK was lost");
        Require(NearlyEqual(loaded.materials[1].alphaCutoff, 0.4f), "MASK cutoff was lost");
        Require(loaded.materials[2].alphaMode == MaterialAlphaMode::Blend, "BLEND was lost");
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
