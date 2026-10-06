// Live material editing (Model Preview's Quick Edit): a previewed edit reaches the scene's render
// submeshes without touching the disk, a revert reads the model back, and a save writes only the
// slots that were edited.
#include <engine/asset/material_definition.h>
#include <engine/asset/material_graph_runtime.h>
#include <engine/asset/model_cache.h>
#include <engine/editor/renderer_shared_state.h>
#include <engine/editor/services/model_import_service.h>
#include <engine/editor/services/scene_renderables.h>
#include <engine/logic/editor_world.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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

bool Near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1.0e-4f;
}

class ScopedDirectory
{
  public:
    ScopedDirectory()
        : path(std::filesystem::temp_directory_path() /
               ("miniengine_material_live_edit_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::filesystem::create_directories(path);
    }

    ~ScopedDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    const std::filesystem::path path;
};

// One triangle per material, each its own mesh: "paint" (blue) and "glass".
std::filesystem::path WriteTwoMaterialModel(const std::filesystem::path& directory)
{
    // Three float3 positions, then three uint16 indices (padded to 4 bytes).
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const unsigned short indices[4] = {0, 1, 2, 0};
    std::ofstream(directory / "car.bin", std::ios::binary)
        .write(reinterpret_cast<const char*>(positions), sizeof(positions))
        .write(reinterpret_cast<const char*>(indices), sizeof(indices));
    const std::filesystem::path model = directory / "car.gltf";
    std::ofstream(model) << R"({
  "asset": { "version": "2.0" },
  "buffers": [ { "uri": "car.bin", "byteLength": 44 } ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0, "byteLength": 36 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 6 } ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0] },
    { "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" } ],
  "materials": [
    { "name": "EXT_Carpaint", "pbrMetallicRoughness": { "baseColorFactor": [0.0, 0.05, 0.4, 1.0], "roughnessFactor": 0.3, "metallicFactor": 0.0 } },
    { "name": "Glass", "pbrMetallicRoughness": { "baseColorFactor": [1.0, 1.0, 1.0, 1.0] } } ],
  "meshes": [
    { "primitives": [ { "attributes": { "POSITION": 0 }, "indices": 1, "material": 0 } ] },
    { "primitives": [ { "attributes": { "POSITION": 0 }, "indices": 1, "material": 1 } ] } ],
  "nodes": [ { "mesh": 0 }, { "mesh": 1 } ],
  "scenes": [ { "nodes": [0, 1] } ],
  "scene": 0
})";
    return model;
}

// The base colour factor the scene draws the paint with.
const float* ScenePaintFactor(RendererSharedState& state)
{
    const auto& submeshes = state.rendererWorld.GetRenderSubmeshes();
    Require(submeshes.size() == 2, "the scene draws the car's two submeshes, got " + std::to_string(submeshes.size()));
    for (const auto& submesh : submeshes)
    {
        if (submesh->material.baseColorFactor[2] < 0.5f)
        {
            return submesh->material.baseColorFactor;
        }
    }
    // Repainted: the submesh that is not the white glass.
    for (const auto& submesh : submeshes)
    {
        if (!(Near(submesh->material.baseColorFactor[0], 1.0f) && Near(submesh->material.baseColorFactor[1], 1.0f)))
        {
            return submesh->material.baseColorFactor;
        }
    }
    throw std::runtime_error("no paint submesh in the scene");
}

void TestPreviewRevertAndSave()
{
    const ScopedDirectory directory;
    const std::filesystem::path modelPath = WriteTwoMaterialModel(directory.path);

    RendererSharedState state;
    state.editorWorld = CreateEditorWorld();
    SerializedEntityData carData;
    carData.tagName = "Car";
    // The scene names the model one way, the Model Preview window another: both must match.
    carData.modelSourcePath = (directory.path / "." / "car.gltf").string();
    state.GetEditorWorld().CreateEntity(carData);
    RebuildSceneRenderables(state);
    Require(Near(ScenePaintFactor(state)[2], 0.4f), "the scene starts with the blue paint");

    // The window's copy of the materials, as BuildEditableMaterials makes it.
    const LoadedModelData loaded = ModelLoader::LoadModel(modelPath.string());
    std::vector<ModelImportedMaterialInfo> materials;
    for (const ModelMaterialData& material : loaded.materials)
    {
        ModelImportedMaterialInfo info = BuildImportedMaterialInfo(material);
        EnsureMaterialShaderGraph(info.name, std::nullopt, info);
        CompileMaterialShaderGraph(info);
        materials.push_back(std::move(info));
    }
    Require(materials.size() == 2 && materials[0].name == "EXT_Carpaint", "the paint is slot 0");

    // Repaint red through the Output node, as Quick Edit does.
    for (MaterialShaderNode& node : materials[0].shaderGraph.nodes)
    {
        if (node.type == MaterialShaderNodeType::Output)
        {
            node.pbr.baseColorFactor[0] = 0.8f;
            node.pbr.baseColorFactor[1] = 0.02f;
            node.pbr.baseColorFactor[2] = 0.02f;
        }
    }
    CompileMaterialShaderGraph(materials[0]);
    Require(Near(materials[0].pbr.roughnessFactor, 0.3f), "the compile keeps the paint's roughness");

    const std::string windowPath = std::filesystem::absolute(modelPath).lexically_normal().string();
    ModelImportService::PreviewImportedModelMaterials(state, windowPath, {{0u, materials[0]}});
    const float* previewed = ScenePaintFactor(state);
    Require(Near(previewed[0], 0.8f) && Near(previewed[2], 0.02f), "the preview repaints the scene's car");
    Require(FindMaterialDefinitionFiles(modelPath).empty(), "a preview writes nothing to disk");

    ModelImportService::RevertImportedModelMaterials(state, windowPath);
    Require(Near(ScenePaintFactor(state)[2], 0.4f), "a revert puts the blue paint back");

    // Save the paint alone: one sidecar, and the scene and a fresh load both red.
    ModelImportService::UpdateImportedModelMaterialDefinitions(state, windowPath, materials, {0u});
    const std::vector<std::filesystem::path> sidecars = FindMaterialDefinitionFiles(modelPath);
    Require(sidecars.size() == 1 && MaterialDefinitionIndex(modelPath, sidecars[0]) == 0u,
            "saving writes only the edited slot's sidecar");
    Require(Near(ScenePaintFactor(state)[0], 0.8f), "the saved paint shows in the scene");
    ModelCache::Invalidate(windowPath);
    const LoadedModelData reloaded = ModelLoader::LoadModel(modelPath.string());
    Require(Near(reloaded.materials[0].baseColor[0], 0.8f) && Near(reloaded.materials[0].roughnessFactor, 0.3f),
            "the saved paint loads back");
    std::cout << "preview, revert and save of a repaint: ok\n";
}
}

int main()
{
    try
    {
        TestPreviewRevertAndSave();
    }
    catch (const std::exception& error)
    {
        std::cerr << "material_live_edit_tests failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
