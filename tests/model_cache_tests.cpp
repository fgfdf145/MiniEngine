#include <engine/asset/model_cache.h>

#include <engine/scene/scene_components.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// A model with `vertexCount` vertices and `indexCount` indices in one submesh,
// carrying one material. Enough for byte accounting and identity checks; the
// geometry is never interpreted.
std::shared_ptr<LoadedModelData> MakeModel(size_t vertexCount, size_t indexCount, const char* materialName)
{
    auto data = std::make_shared<LoadedModelData>();
    ModelSubmeshData submesh;
    submesh.mesh.vertices.resize(vertexCount);
    submesh.mesh.indices.resize(indexCount);
    data->submeshes.push_back(std::move(submesh));

    ModelMaterialData material;
    material.name = materialName;
    data->materials.push_back(std::move(material));
    return data;
}

void UpdateEntryPoints()
{
    const std::string path = "C:/fake/update.glb";
    ModelCache::Invalidate(path);
    ModelCache::Store(path, MakeModel(3, 3, "original"));

    ModelImportedMaterialInfo info;
    info.name = "edited";
    ModelCache::UpdateMaterial(path, 0, info);

    std::shared_ptr<const LoadedModelData> observed = ModelCache::Get(path);
    Require(observed != nullptr, "cached model vanished after UpdateMaterial");
    Require(observed->materials.size() == 1, "UpdateMaterial changed the material count");
    Require(observed->materials[0].name == "edited", "UpdateMaterial did not reach the cached model");

    // Out-of-range index and absent path are both no-ops, not crashes.
    ModelCache::UpdateMaterial(path, 99, info);
    ModelCache::UpdateMaterial("C:/fake/not-cached.glb", 0, info);

    std::vector<ModelImportedMaterialInfo> batch;
    ModelImportedMaterialInfo second;
    second.name = "batch-edited";
    batch.push_back(second);
    ModelCache::UpdateMaterials(path, batch);
    observed = ModelCache::Get(path);
    Require(observed->materials[0].name == "batch-edited", "UpdateMaterials did not reach the cached model");

    ModelCache::UpdateMaterials("C:/fake/not-cached.glb", batch);

    ModelCache::Invalidate(path);
    Require(ModelCache::Get(path) == nullptr, "Invalidate did not remove the entry");
}
}

int main()
{
    try
    {
        UpdateEntryPoints();

        std::cout << "model cache tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "model cache tests failed: " << error.what() << '\n';
        return 1;
    }
}
