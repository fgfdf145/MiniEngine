#include <material_graph.h>
#include <material_definition.h>
#include <material_graph_runtime.h>
#include <material_pipeline.h>
#include <model_loader.h>
#include <renderer_world.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

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
        const std::filesystem::path path = temporaryDirectory / ("miniengine_material_alpha_" +
                                                                 std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()) + "_" +
                                                                 std::to_string(randomDevice()));
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
    const std::array<float, 9> positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const std::array<uint16_t, 3> indices = {0, 1, 2};
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
        for (const float nonFinite : {
                 std::numeric_limits<float>::quiet_NaN(),
                 std::numeric_limits<float>::infinity(),
                 -std::numeric_limits<float>::infinity()})
        {
            const float normalized = ClampMaterialAlphaValue(nonFinite);
            Require(
                std::isfinite(normalized) && normalized >= 0.0f && normalized <= 1.0f,
                "non-finite alpha helper result escaped normalization");
        }
        Require(NearlyEqual(
                    ResolveMaterialCoverageAlpha(MaterialAlphaMode::Opaque, 0.2f, 0.5f),
                    1.0f),
                "Opaque did not force full coverage");
        Require(NearlyEqual(
                    ResolveMaterialCoverageAlpha(MaterialAlphaMode::Mask, 0.49f, 0.5f),
                    0.0f),
                "Mask did not discard below cutoff");
        Require(NearlyEqual(
                    ResolveMaterialCoverageAlpha(MaterialAlphaMode::Mask, 0.5f, 0.5f),
                    1.0f),
                "Mask incorrectly discarded at cutoff");
        Require(NearlyEqual(
                    ResolveMaterialCoverageAlpha(MaterialAlphaMode::Blend, 0.25f, 0.5f),
                    0.25f),
                "Blend did not preserve alpha");

        const MaterialPipelineState opaqueState = GetMaterialPipelineState({MaterialAlphaMode::Opaque, false});
        Require(!opaqueState.blendEnabled, "Opaque blending was enabled");
        Require(opaqueState.depthWriteEnabled, "Opaque depth writes were disabled");
        Require(!opaqueState.alphaMaskEnabled, "Opaque mask discard was enabled");
        Require(opaqueState.cullBackFaces, "single-sided Opaque stopped culling");
        Require(!opaqueState.writeAttachmentAlpha, "Opaque wrote fractional viewport alpha");

        const MaterialPipelineState maskState = GetMaterialPipelineState({MaterialAlphaMode::Mask, true});
        Require(!maskState.blendEnabled, "Mask blending was enabled");
        Require(maskState.depthWriteEnabled, "Mask depth writes were disabled");
        Require(maskState.alphaMaskEnabled, "Mask discard was disabled");
        Require(!maskState.cullBackFaces, "double-sided Mask still culled");
        Require(!maskState.writeAttachmentAlpha, "Mask wrote fractional viewport alpha");

        const MaterialPipelineState blendState = GetMaterialPipelineState({MaterialAlphaMode::Blend, false});
        Require(blendState.blendEnabled, "Blend blending was disabled");
        Require(!blendState.depthWriteEnabled, "Blend depth writes were enabled");
        Require(!blendState.alphaMaskEnabled, "Blend mask discard was enabled");
        Require(blendState.writeAttachmentAlpha, "Blend did not write attachment alpha");

        const std::array<MaterialDrawSortKey, 6> sortKeys{{{{MaterialAlphaMode::Blend, false}, 2.0f},
                                                           {{MaterialAlphaMode::Mask, false}, 8.0f},
                                                           {{MaterialAlphaMode::Blend, true}, 9.0f},
                                                           {{MaterialAlphaMode::Opaque, true}, 1.0f},
                                                           {{MaterialAlphaMode::Blend, false}, 9.0f},
                                                           {{MaterialAlphaMode::Blend, false}, 4.0f}}};
        const std::vector<size_t> order = BuildMaterialDrawOrder(sortKeys);
        Require(order == std::vector<size_t>({1, 3, 2, 4, 5, 0}), "draw order mismatch");

        const float nanDepth = std::numeric_limits<float>::quiet_NaN();
        const std::array<MaterialDrawSortKey, 6> nanSortKeys{{{{MaterialAlphaMode::Blend, false}, nanDepth},
                                                              {{MaterialAlphaMode::Blend, false}, 2.0f},
                                                              {{MaterialAlphaMode::Blend, true}, nanDepth},
                                                              {{MaterialAlphaMode::Blend, false}, std::numeric_limits<float>::infinity()},
                                                              {{MaterialAlphaMode::Blend, false}, -std::numeric_limits<float>::infinity()},
                                                              {{MaterialAlphaMode::Blend, true}, 2.0f}}};
        const std::vector<size_t> nanOrder = BuildMaterialDrawOrder(nanSortKeys);
        Require(
            nanOrder == std::vector<size_t>({3, 1, 5, 4, 0, 2}),
            "NaN blend draw order mismatch");

        MeshData boundsMesh{};
        boundsMesh.vertices = {
            {{-2.0f, -4.0f, -6.0f}},
            {{6.0f, 8.0f, 10.0f}}};
        Require(
            glm::length(ComputeMeshBoundsCenter(boundsMesh) - glm::vec3(2.0f, 2.0f, 2.0f)) < 0.0001f,
            "mesh bounds center mismatch");
        Require(
            ComputeMeshBoundsCenter(MeshData{}) == glm::vec3(0.0f),
            "empty mesh did not use local origin");

        ModelImportedMaterialInfo source{};
        source.name = "masked foliage";
        source.pbr.alphaMode = MaterialAlphaMode::Mask;
        source.pbr.alphaCutoff = 0.37f;
        source.pbr.baseColorFactor[3] = 0.6f;
        source.pbr.opacity = 0.8f;

        const YAML::Node serialized = SerializeMaterialDefinition(source);
        Require(
            serialized["pbr"]["alpha_mode"].as<std::string>() == "mask",
            "sidecar alpha mode was not canonical");
        Require(
            NearlyEqual(serialized["pbr"]["alpha_cutoff"].as<float>(), 0.37f),
            "sidecar cutoff was not serialized");

        const ScopedFixtureDirectory sidecarFixtureDirectory;
        const std::filesystem::path sidecar = sidecarFixtureDirectory.path / "material_alpha.material.yaml";
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

        for (const char* nonFiniteToken : {".nan", "+.inf", "-.inf"})
        {
            std::ofstream file(sidecar);
            file << "material:\n  pbr:\n    alpha_cutoff: " << nonFiniteToken
                 << "\n    opacity: " << nonFiniteToken << '\n';
            file.close();

            restored = {};
            restored.pbr.alphaCutoff = 0.34f;
            restored.pbr.opacity = 0.76f;
            warning.clear();
            Require(LoadMaterialDefinition(sidecar, restored, warning), "non-finite sidecar did not load");
            Require(
                NearlyEqual(restored.pbr.alphaCutoff, 0.34f),
                "non-finite sidecar cutoff did not retain imported fallback");
            Require(
                NearlyEqual(restored.pbr.opacity, 0.76f),
                "non-finite sidecar opacity did not retain imported fallback");
            Require(
                std::isfinite(restored.pbr.alphaCutoff) &&
                    restored.pbr.alphaCutoff >= 0.0f && restored.pbr.alphaCutoff <= 1.0f,
                "non-finite sidecar cutoff escaped runtime normalization");
            Require(
                std::isfinite(restored.pbr.opacity) &&
                    restored.pbr.opacity >= 0.0f && restored.pbr.opacity <= 1.0f,
                "non-finite sidecar opacity escaped runtime normalization");
        }

        {
            std::ofstream file(sidecar);
            file << "material:\n  pbr:\n    alpha_cutoff: .nan\n    opacity: .nan\n";
        }
        restored = {};
        restored.pbr.alphaCutoff = std::numeric_limits<float>::quiet_NaN();
        restored.pbr.opacity = std::numeric_limits<float>::infinity();
        warning.clear();
        Require(LoadMaterialDefinition(sidecar, restored, warning), "defaulted non-finite sidecar did not load");
        Require(NearlyEqual(restored.pbr.alphaCutoff, 0.5f), "invalid cutoff fallback was not 0.5");
        Require(NearlyEqual(restored.pbr.opacity, 1.0f), "invalid opacity fallback was not 1.0");

        ModelImportedMaterialInfo nonFiniteSource{};
        nonFiniteSource.pbr.alphaCutoff = std::numeric_limits<float>::quiet_NaN();
        nonFiniteSource.pbr.opacity = std::numeric_limits<float>::infinity();
        const YAML::Node nonFiniteSerialized = SerializeMaterialDefinition(nonFiniteSource);
        const float serializedCutoff = nonFiniteSerialized["pbr"]["alpha_cutoff"].as<float>();
        const float serializedOpacity = nonFiniteSerialized["pbr"]["opacity"].as<float>();
        Require(
            std::isfinite(serializedCutoff) && serializedCutoff >= 0.0f && serializedCutoff <= 1.0f,
            "serializer emitted non-finite cutoff");
        Require(
            std::isfinite(serializedOpacity) && serializedOpacity >= 0.0f && serializedOpacity <= 1.0f,
            "serializer emitted non-finite opacity");
        Require(
            YAML::Dump(nonFiniteSerialized).find(".nan") == std::string::npos,
            "serializer emitted .nan");
        std::error_code removeError;

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
            "legacy sidecar lost alpha mode");
        Require(
            NearlyEqual(legacyReloaded.materials[1].pbr.alphaCutoff, 0.4f),
            "legacy sidecar lost alpha cutoff");
        std::filesystem::remove(legacySidecar, removeError);

        const ScopedFixtureDirectory legacyGraphFixtureDirectory;
        const std::filesystem::path legacyGraphSidecar =
            legacyGraphFixtureDirectory.path / "legacy_graph.material.yaml";
        {
            std::ofstream file(legacyGraphSidecar);
            file << "material:\n"
                    "  name: legacy graph\n"
                    "  pbr:\n"
                    "    alpha_mode: mask\n"
                    "    alpha_cutoff: 0.41\n"
                    "  shader_graph:\n"
                    "    version: 2\n"
                    "    next_node_id: 2\n"
                    "    next_link_id: 1\n"
                    "    nodes:\n"
                    "      - id: 1\n"
                    "        type: output\n"
                    "        name: Legacy Output\n"
                    "        pbr:\n"
                    "          base_color_factor: [1, 1, 1, 0.7]\n"
                    "          opacity: 0.8\n"
                    "    links: []\n";
        }
        ModelImportedMaterialInfo legacyGraphMaterial{};
        warning.clear();
        Require(
            LoadMaterialDefinition(legacyGraphSidecar, legacyGraphMaterial, warning),
            "legacy shader graph sidecar did not load");
        Require(CompileMaterialShaderGraph(legacyGraphMaterial).success, "legacy shader graph did not compile");

        const std::filesystem::path savedGraphSidecar =
            legacyGraphFixtureDirectory.path / "saved_graph.material.yaml";
        {
            YAML::Node root(YAML::NodeType::Map);
            root["material"] = SerializeMaterialDefinition(legacyGraphMaterial);
            std::ofstream file(savedGraphSidecar);
            file << root;
        }
        ModelImportedMaterialInfo savedGraphMaterial{};
        warning.clear();
        Require(
            LoadMaterialDefinition(savedGraphSidecar, savedGraphMaterial, warning),
            "saved shader graph sidecar did not reload");
        Require(CompileMaterialShaderGraph(savedGraphMaterial).success, "saved shader graph did not compile");
        Require(
            savedGraphMaterial.pbr.alphaMode == MaterialAlphaMode::Mask,
            "legacy shader graph lost canonical alpha mode");
        Require(
            NearlyEqual(savedGraphMaterial.pbr.alphaCutoff, 0.41f),
            "legacy shader graph lost canonical alpha cutoff");

        const ScopedFixtureDirectory scalarGraphFixtureDirectory;
        const std::filesystem::path scalarGraphSidecar =
            scalarGraphFixtureDirectory.path / "nan_scalar.material.yaml";
        {
            std::ofstream file(scalarGraphSidecar);
            file << "material:\n"
                    "  name: nan scalar graph\n"
                    "  pbr:\n"
                    "    alpha_mode: blend\n"
                    "    opacity: 0.6\n"
                    "  shader_graph:\n"
                    "    version: 2\n"
                    "    next_node_id: 4\n"
                    "    next_link_id: 3\n"
                    "    nodes:\n"
                    "      - id: 1\n"
                    "        type: output\n"
                    "        name: Output\n"
                    "      - id: 2\n"
                    "        type: scalar\n"
                    "        name: Invalid Opacity\n"
                    "        scalar_value: .nan\n"
                    "      - id: 3\n"
                    "        type: scalar\n"
                    "        name: High Emissive\n"
                    "        scalar_value: 4.0\n"
                    "    links:\n"
                    "      - id: 1\n"
                    "        from_node_id: 2\n"
                    "        from_slot: value\n"
                    "        to_node_id: 1\n"
                    "        to_slot: opacity\n"
                    "      - id: 2\n"
                    "        from_node_id: 3\n"
                    "        from_slot: value\n"
                    "        to_node_id: 1\n"
                    "        to_slot: emissive_intensity\n";
        }
        ModelImportedMaterialInfo scalarGraphMaterial{};
        warning.clear();
        Require(
            LoadMaterialDefinition(scalarGraphSidecar, scalarGraphMaterial, warning),
            "NaN scalar graph sidecar did not load");
        Require(CompileMaterialShaderGraph(scalarGraphMaterial).success, "NaN scalar graph did not compile");
        Require(
            std::isfinite(scalarGraphMaterial.pbr.opacity) &&
                scalarGraphMaterial.pbr.opacity >= 0.0f && scalarGraphMaterial.pbr.opacity <= 1.0f,
            "connected scalar produced non-finite compiled opacity");
        Require(
            NearlyEqual(scalarGraphMaterial.pbr.emissiveIntensity, 4.0f),
            "finite generic scalar was saturated during compile");

        YAML::Node scalarGraphRoot(YAML::NodeType::Map);
        scalarGraphRoot["material"] = SerializeMaterialDefinition(scalarGraphMaterial);
        const YAML::Node scalarGraphSerialized = scalarGraphRoot["material"];
        const float serializedRootOpacity = scalarGraphSerialized["pbr"]["opacity"].as<float>();
        Require(
            std::isfinite(serializedRootOpacity) &&
                serializedRootOpacity >= 0.0f && serializedRootOpacity <= 1.0f,
            "serialized root opacity was non-finite");
        bool foundScalar = false;
        bool allScalarsFinite = true;
        bool preservedHighScalar = false;
        for (const YAML::Node& shaderNode : scalarGraphSerialized["shader_graph"]["nodes"])
        {
            if (shaderNode["type"].as<std::string>("") == "scalar")
            {
                const float scalarValue = shaderNode["scalar_value"].as<float>();
                foundScalar = true;
                allScalarsFinite &= std::isfinite(scalarValue);
                if (shaderNode["name"].as<std::string>("") == "High Emissive")
                {
                    preservedHighScalar = NearlyEqual(scalarValue, 4.0f);
                }
            }
        }
        Require(foundScalar && allScalarsFinite, "serialized scalar remained non-finite");
        Require(preservedHighScalar, "serializer saturated finite generic scalar");
        Require(YAML::Dump(scalarGraphRoot).find(".nan") == std::string::npos, "saved scalar graph emitted .nan");

        const std::filesystem::path savedScalarGraphSidecar =
            scalarGraphFixtureDirectory.path / "saved_nan_scalar.material.yaml";
        {
            std::ofstream file(savedScalarGraphSidecar);
            file << scalarGraphRoot;
        }
        ModelImportedMaterialInfo savedScalarGraphMaterial{};
        warning.clear();
        Require(
            LoadMaterialDefinition(savedScalarGraphSidecar, savedScalarGraphMaterial, warning),
            "saved scalar graph sidecar did not reload");
        Require(CompileMaterialShaderGraph(savedScalarGraphMaterial).success, "saved scalar graph did not compile");
        Require(
            std::isfinite(savedScalarGraphMaterial.pbr.opacity) &&
                savedScalarGraphMaterial.pbr.opacity >= 0.0f && savedScalarGraphMaterial.pbr.opacity <= 1.0f,
            "reloaded scalar graph opacity was non-finite");
        Require(
            NearlyEqual(savedScalarGraphMaterial.pbr.emissiveIntensity, 4.0f),
            "reloaded generic scalar lost its finite value");
        bool reloadedHighScalar = false;
        for (const MaterialShaderNode& shaderNode : savedScalarGraphMaterial.shaderGraph.nodes)
        {
            if (shaderNode.type == MaterialShaderNodeType::Scalar)
            {
                Require(std::isfinite(shaderNode.scalarValue), "reloaded scalar remained non-finite");
                if (shaderNode.name == "High Emissive")
                {
                    reloadedHighScalar = NearlyEqual(shaderNode.scalarValue, 4.0f);
                }
            }
        }
        Require(reloadedHighScalar, "reloaded finite generic scalar was saturated");

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
