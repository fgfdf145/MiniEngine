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

void ByteAccounting()
{
    const LoadedModelData data = *MakeModel(10, 20, "m");
    const size_t expected = 10 * sizeof(Vertex) + 20 * sizeof(uint32_t);
    Require(ModelCache::EstimateBytes(data) == expected, "EstimateBytes did not match vertices+indices");

    // TotalBytes reports what the cache actually holds, so a stored model of
    // known size is the one case where its value is fully determined.
    const std::string path = "C:/fake/bytes.glb";
    ModelCache::Invalidate(path);
    const size_t before = ModelCache::TotalBytes();
    ModelCache::Store(path, MakeModel(10, 20, "m"));
    Require(ModelCache::TotalBytes() == before + expected, "TotalBytes did not account for a stored model");
    ModelCache::Invalidate(path);
    Require(ModelCache::TotalBytes() == before, "TotalBytes did not drop after Invalidate");
}

void LiveKeysArePinned()
{
    const std::string live = "C:/fake/live.glb";
    ModelCache::Invalidate(live);
    ModelCache::Store(live, MakeModel(100, 100, "m"));

    // Budget of zero: everything unreferenced goes, everything live stays.
    ModelCache::Trim({live}, 0);
    Require(ModelCache::Get(live) != nullptr, "a live key was evicted despite a zero budget");

    ModelCache::Trim({}, 0);
    Require(ModelCache::Get(live) == nullptr, "an unreferenced entry survived a zero budget");
}

void LruOrder()
{
    const std::string a = "C:/fake/a.glb";
    const std::string b = "C:/fake/b.glb";
    const std::string c = "C:/fake/c.glb";
    for (const std::string& path : {a, b, c})
    {
        ModelCache::Invalidate(path);
    }

    // Equal size each, so the budget arithmetic is unambiguous.
    ModelCache::Store(a, MakeModel(10, 10, "m"));
    ModelCache::Store(b, MakeModel(10, 10, "m"));
    ModelCache::Store(c, MakeModel(10, 10, "m"));
    const size_t one = 10 * sizeof(Vertex) + 10 * sizeof(uint32_t);

    // Touch a and c, leaving b as least recently used.
    Require(ModelCache::Get(a) != nullptr, "a was not stored");
    Require(ModelCache::Get(c) != nullptr, "c was not stored");

    ModelCache::Trim({}, one * 2);
    Require(ModelCache::Get(b) == nullptr, "LRU did not evict the least recently used entry");
    Require(ModelCache::Get(a) != nullptr, "LRU evicted a recently used entry");
    Require(ModelCache::Get(c) != nullptr, "LRU evicted a recently used entry");

    ModelCache::Invalidate(a);
    ModelCache::Invalidate(c);
}

// Trim normalizes what it is given, so a live key spelled differently from
// the stored key still pins its entry. Backslashes only separate on Windows.
void LiveKeyNormalization()
{
    const std::string stored = "C:/fake/norm/model.glb";
    ModelCache::Invalidate(stored);
    ModelCache::Store(stored, MakeModel(10, 10, "m"));

#ifdef _WIN32
    ModelCache::Trim({"C:\\fake\\norm\\model.glb"}, 0);
#else
    ModelCache::Trim({"C:/fake/./norm//model.glb"}, 0);
#endif
    Require(ModelCache::Get(stored) != nullptr, "a differently-spelled live key failed to pin its entry");

    ModelCache::Invalidate(stored);
}
}

int main()
{
    try
    {
        UpdateEntryPoints();
        ByteAccounting();
        LiveKeysArePinned();
        LruOrder();
        LiveKeyNormalization();

        std::cout << "model cache tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "model cache tests failed: " << error.what() << '\n';
        return 1;
    }
}
