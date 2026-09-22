#include <engine/asset/model_import_target.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>

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

class ScopedDir
{
  public:
    explicit ScopedDir(const char* name)
    {
        std::error_code ec;
        const std::filesystem::path base =
            std::filesystem::canonical(std::filesystem::temp_directory_path(), ec);
        m_path = (ec ? std::filesystem::temp_directory_path() : base) / "miniengine_model_import_target_tests" / name;
        std::filesystem::remove_all(m_path, ec);
        std::filesystem::create_directories(m_path, ec);
    }

    ~ScopedDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }

    ScopedDir(const ScopedDir&) = delete;
    ScopedDir& operator=(const ScopedDir&) = delete;

    const std::filesystem::path& Path() const
    {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
};

void WriteFile(const std::filesystem::path& path, const std::string& content)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool Exists(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

void DefaultFolderIsNamedAfterTheModel()
{
    const std::filesystem::path source = "C:/downloads/scene.gltf";
    Require(
        ModelImportTarget::DefaultFolder(source, "C:/assets/models") == std::filesystem::path("C:/assets/models/scene"),
        "import did not get its own folder named after the model");
    Require(
        ModelImportTarget::DefaultFolder(source, "C:/assets/scene") == std::filesystem::path("C:/assets/scene"),
        "importing into a folder carrying the model's name nested another level");
}

void OccupiedMeansHoldsSomething()
{
    ScopedDir scope("occupied");
    const std::filesystem::path folder = scope.Path() / "scene";
    Require(!ModelImportTarget::IsOccupied(folder), "a missing folder counted as occupied");

    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    Require(!ModelImportTarget::IsOccupied(folder), "an empty folder counted as occupied");

    WriteFile(folder / "scene.gltf", "{}");
    Require(ModelImportTarget::IsOccupied(folder), "a folder holding a model did not count as occupied");
}

void NextFreeFolderSkipsOccupiedSiblings()
{
    ScopedDir scope("next_free");
    WriteFile(scope.Path() / "scene" / "scene.gltf", "{}");
    WriteFile(scope.Path() / "scene_1" / "scene.gltf", "{}");

    Require(
        ModelImportTarget::NextFreeFolder(scope.Path() / "scene") == scope.Path() / "scene_2",
        "keep-both did not pick the first free numbered sibling");
}

// Overwrite replaces the files but keeps the model's uuid sidecar, so scene
// references to the re-imported model keep resolving.
void ReplaceContentsKeepsUuidSidecars()
{
    ScopedDir scope("replace");
    const std::filesystem::path target = scope.Path() / "scene";
    WriteFile(target / "scene.gltf", "old model");
    WriteFile(target / "scene.gltf.miniengine_asset.yaml", "asset: {uuid: kept}");
    WriteFile(target / "scene_0.material.yaml", "old material edit");
    WriteFile(target / "textures" / "old.png", "old texture");

    const std::filesystem::path staging = ModelImportTarget::StagingFolderFor(target);
    Require(!Exists(staging), "staging folder already exists");
    WriteFile(staging / "scene.gltf", "new model");
    WriteFile(staging / "textures" / "new.png", "new texture");

    ModelImportTarget::ReplaceContents(target, staging);

    Require(ReadAll(target / "scene.gltf") == "new model", "model was not replaced");
    Require(
        ReadAll(target / "scene.gltf.miniengine_asset.yaml") == "asset: {uuid: kept}",
        "the model's uuid sidecar was not kept");
    Require(!Exists(target / "scene_0.material.yaml"), "old material edits survived the overwrite");
    Require(!Exists(target / "textures" / "old.png"), "old texture survived the overwrite");
    Require(ReadAll(target / "textures" / "new.png") == "new texture", "new texture was not moved in");
    Require(!Exists(staging), "staging folder was left behind");
}

void ReplaceContentsPrunesEmptiedDirectories()
{
    ScopedDir scope("prune");
    const std::filesystem::path target = scope.Path() / "scene";
    WriteFile(target / "buffers" / "scene.bin", "old buffer");

    const std::filesystem::path staging = ModelImportTarget::StagingFolderFor(target);
    WriteFile(staging / "scene.glb", "self-contained");

    ModelImportTarget::ReplaceContents(target, staging);

    Require(!Exists(target / "buffers"), "a directory emptied by the overwrite was left behind");
    Require(Exists(target / "scene.glb"), "new model was not moved in");
}
}

int main()
{
    try
    {
        DefaultFolderIsNamedAfterTheModel();
        OccupiedMeansHoldsSomething();
        NextFreeFolderSkipsOccupiedSiblings();
        ReplaceContentsKeepsUuidSidecars();
        ReplaceContentsPrunesEmptiedDirectories();

        std::cout << "model import target tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "model import target tests failed: " << error.what() << '\n';
        return 1;
    }
}
