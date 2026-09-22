#include <engine/asset/asset_registry.h>
#include <engine/editor/renderer_shared_state.h>
#include <engine/editor/services/model_import_service.h>
#include <engine/logic/editor_world.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>

// main() stays in the global namespace; everything it drives lives in me::.
using namespace me;

// Drives the editor's import, paste and delete services against a scratch
// asset tree, end to end on the real filesystem.
namespace
{
void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// A single triangle with an embedded 1x1 PNG, all as data URIs; import
// unpacks the texture into textures/.
constexpr const char* kFixtureGltf = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [ 0 ] } ],
  "nodes": [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0, "TEXCOORD_0": 1 }, "indices": 2, "material": 0 } ] } ],
  "materials": [ { "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } } ],
  "textures": [ { "source": 0 } ],
  "images": [ { "name": "", "mimeType": "image/png", "uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC" } ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,1,0] },
    { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2" },
    { "bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR" }
  ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0, "byteLength": 36 },
    { "buffer": 0, "byteOffset": 36, "byteLength": 24 },
    { "buffer": 0, "byteOffset": 60, "byteLength": 6 }
  ],
  "buffers": [ { "byteLength": 68, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAABAAIAAAA=" } ]
})";

class ScratchTree
{
  public:
    explicit ScratchTree(const char* name)
    {
        std::error_code ec;
        const std::filesystem::path base =
            std::filesystem::canonical(std::filesystem::temp_directory_path(), ec);
        m_root = (ec ? std::filesystem::temp_directory_path() : base) / "miniengine_asset_file_operations_tests" / name;
        std::filesystem::remove_all(m_root, ec);
        std::filesystem::create_directories(Assets(), ec);
        WriteFile(Source(), kFixtureGltf);
        AssetRegistry::Initialize(Assets());
    }

    ~ScratchTree()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_root, ec);
    }

    ScratchTree(const ScratchTree&) = delete;
    ScratchTree& operator=(const ScratchTree&) = delete;

    std::filesystem::path Assets() const
    {
        return m_root / "assets";
    }
    std::filesystem::path Source() const
    {
        return m_root / "downloads" / "fixture.gltf";
    }

    static void WriteFile(const std::filesystem::path& path, const std::string& content)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << content;
    }

  private:
    std::filesystem::path m_root;
};

bool Exists(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

template <typename Action>
bool Throws(Action action)
{
    try
    {
        action();
    }
    catch (const std::exception&)
    {
        return true;
    }
    return false;
}

std::string Import(const ScratchTree& tree, ImportConflictPolicy policy)
{
    return ModelImportService::ImportModelIntoAssetDirectory(
        tree.Source().string(), tree.Assets().string(), policy);
}

void SecondImportAsksAndKeepsBoth()
{
    ScratchTree tree("keep_both");
    const std::filesystem::path first = Import(tree, ImportConflictPolicy::FailIfExists);
    Require(first == tree.Assets() / "fixture" / "fixture.gltf", "first import landed in the wrong folder");

    Require(
        Throws([&]
               {
                   Import(tree, ImportConflictPolicy::FailIfExists);
               }),
        "a second import silently reused the existing folder");

    const std::filesystem::path second = Import(tree, ImportConflictPolicy::KeepBoth);
    Require(second == tree.Assets() / "fixture_1" / "fixture.gltf", "keep-both did not import into fixture_1");
    Require(Exists(first), "keep-both touched the existing model");
}

void OverwriteReplacesFilesAndKeepsUuid()
{
    ScratchTree tree("overwrite");
    const std::filesystem::path model = Import(tree, ImportConflictPolicy::FailIfExists);
    const std::string uuid = AssetRegistry::GetOrCreateUuid(model);
    Require(!uuid.empty(), "imported model got no uuid");
    ScratchTree::WriteFile(model.parent_path() / "fixture_0.material.yaml", "material: {name: edited}");

    const std::filesystem::path replaced = Import(tree, ImportConflictPolicy::Overwrite);
    Require(replaced == model, "overwrite imported somewhere else");
    Require(Exists(model), "overwrite lost the model");
    Require(Exists(model.parent_path() / "textures"), "overwrite lost the unpacked textures");
    Require(!Exists(model.parent_path() / "fixture_0.material.yaml"), "old material edits survived the overwrite");
    Require(!Exists(tree.Assets() / "fixture.importing"), "the staging folder was left behind");
    Require(AssetRegistry::GetOrCreateUuid(model) == uuid, "overwrite changed the model's uuid");
}

void FailedOverwriteKeepsTheOldModel()
{
    ScratchTree tree("overwrite_failure");
    const std::filesystem::path model = Import(tree, ImportConflictPolicy::FailIfExists);
    const std::string before = ReadAll(model);

    ScratchTree::WriteFile(tree.Source(), "this is not json");
    Require(
        Throws([&]
               {
                   Import(tree, ImportConflictPolicy::Overwrite);
               }),
        "importing a broken model reported success");
    Require(ReadAll(model) == before, "a failed overwrite damaged the existing model");
    Require(!Exists(tree.Assets() / "fixture.importing"), "a failed overwrite left its staging folder");
}

void PastedModelBringsItsCompanions()
{
    ScratchTree tree("paste_model");
    const std::filesystem::path model = Import(tree, ImportConflictPolicy::FailIfExists);
    ScratchTree::WriteFile(model.parent_path() / "fixture_0.material.yaml", "material: {name: edited}");
    const std::filesystem::path other = tree.Assets() / "other";
    std::error_code ec;
    std::filesystem::create_directories(other, ec);

    ModelImportService::PasteAsset(model.string(), other.string());
    const std::filesystem::path copy = other / "fixture";
    Require(Exists(copy / "fixture.gltf"), "pasted model is missing");
    Require(Exists(copy / "textures"), "pasted model lost its textures");
    Require(
        ReadAll(copy / "fixture_0.material.yaml") == "material: {name: edited}",
        "pasted model lost its material edits");

    ModelImportService::PasteAsset(model.string(), other.string());
    Require(Exists(other / "fixture_1" / "fixture.gltf"), "second paste did not get its own folder");
}

void PastedFileNeverOverwrites()
{
    ScratchTree tree("paste_file");
    const std::filesystem::path file = tree.Assets() / "notes.txt";
    ScratchTree::WriteFile(file, "original");

    ModelImportService::PasteAsset(file.string(), tree.Assets().string());
    Require(ReadAll(tree.Assets() / "notes_copy.txt") == "original", "pasting next to itself made no copy");
    ModelImportService::PasteAsset(file.string(), tree.Assets().string());
    Require(Exists(tree.Assets() / "notes_copy2.txt"), "second paste did not pick the next free name");
}

void FolderCannotBePastedIntoItself()
{
    ScratchTree tree("paste_into_self");
    const std::filesystem::path folder = tree.Assets() / "Tree";
    ScratchTree::WriteFile(folder / "leaves" / "a.txt", "x");

    Require(
        Throws([&]
               {
                   ModelImportService::PasteAsset(folder.string(), (folder / "leaves").string());
               }),
        "a folder was pasted into its own child");
    Require(!Exists(folder / "leaves" / "Tree"), "the refused paste still copied something");
}

void DeleteRemovesModelThenMaterialEdits()
{
    ScratchTree tree("delete");
    const std::filesystem::path model = Import(tree, ImportConflictPolicy::FailIfExists);
    const std::filesystem::path edits = model.parent_path() / "fixture_0.material.yaml";
    ScratchTree::WriteFile(edits, "material: {name: edited}");

#ifdef _WIN32
    {
        // An open handle without delete sharing makes the delete fail; the
        // material edits must still be there afterwards.
        std::ifstream holder(model, std::ios::binary);
        Require(
            Throws([&]
                   {
                       ModelImportService::DeleteAssetPath(model.string());
                   }),
            "deleting a model held open reported success");
        Require(Exists(edits), "a failed model delete took its material edits");
    }
#endif

    ModelImportService::DeleteAssetPath(model.string());
    Require(!Exists(model), "model was not deleted");
    Require(!Exists(edits), "material edits outlived their model");
}

// An open scene follows a rename: otherwise the entity's next reload fails and
// its material edits are saved under the old name.
void RenameRetargetsSceneReferences()
{
    RendererSharedState state;
    state.editorWorld = CreateEditorWorld();

    SerializedEntityData inside;
    inside.modelSourcePath = "C:/assets/Tree/tree.gltf";
    inside.modelDisplayName = "tree.gltf";
    inside.modelBaseColorTextureOverridePath = "C:/assets/Tree/textures/bark.png";
    SerializedEntityData outside;
    outside.modelSourcePath = "C:/assets/Trees/tree.gltf";
    const entt::entity insideEntity = state.GetEditorWorld().CreateEntity(inside);
    const entt::entity outsideEntity = state.GetEditorWorld().CreateEntity(outside);

    ModelImportService::OnAssetRenamed(state, "C:/assets/Tree", "C:/assets/Oak");

    const ModelComponent& moved = state.GetEditorWorld().GetModel(insideEntity);
    Require(
        std::filesystem::path(moved.sourcePath) == std::filesystem::path("C:/assets/Oak/tree.gltf").lexically_normal(),
        "model inside the renamed folder kept its old path");
    Require(
        std::filesystem::path(moved.baseColorTextureOverridePath) ==
            std::filesystem::path("C:/assets/Oak/textures/bark.png").lexically_normal(),
        "texture override inside the renamed folder kept its old path");
    Require(
        state.GetEditorWorld().GetModel(outsideEntity).sourcePath == "C:/assets/Trees/tree.gltf",
        "a model outside the renamed folder was retargeted");

    ModelImportService::OnAssetRenamed(state, "C:/assets/Oak/tree.gltf", "C:/assets/Oak/oak.gltf");
    const ModelComponent& renamed = state.GetEditorWorld().GetModel(insideEntity);
    Require(
        std::filesystem::path(renamed.sourcePath) == std::filesystem::path("C:/assets/Oak/oak.gltf"),
        "renamed model file kept its old path");
    Require(renamed.displayName == "oak.gltf", "display name still shows the old file name");
}
}

int main()
{
    try
    {
        SecondImportAsksAndKeepsBoth();
        OverwriteReplacesFilesAndKeepsUuid();
        FailedOverwriteKeepsTheOldModel();
        PastedModelBringsItsCompanions();
        PastedFileNeverOverwrites();
        FolderCannotBePastedIntoItself();
        DeleteRemovesModelThenMaterialEdits();
        RenameRetargetsSceneReferences();

        std::cout << "asset file operations tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "asset file operations tests failed: " << error.what() << '\n';
        return 1;
    }
}
