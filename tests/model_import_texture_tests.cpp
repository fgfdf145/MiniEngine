#include <engine/asset/gltf_model_loader.h>
#include <engine/asset/model_loader.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

// A 1x1 red PNG and a single-triangle buffer, both as base64 data URIs, so the
// fixture is a string in the test rather than a checked-in binary.
constexpr const char* kEmbeddedPngBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC";
constexpr const char* kTriangleBufferBase64 =
    "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAABAAIAAAA=";

// Positions at bytes 0..35, UVs at 36..59, indices at 60..65; the buffer is 68
// bytes so every accessor offset stays aligned to its component size.
std::string BuildFixtureGltf()
{
    std::string gltf = R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [ 0 ] } ],
  "nodes": [ { "mesh": 0 } ],
  "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0, "TEXCOORD_0": 1 }, "indices": 2, "material": 0 } ] } ],
  "materials": [ { "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } } ],
  "textures": [ { "source": 0 } ],
  "images": [ { "name": "", "mimeType": "image/png", "uri": "data:image/png;base64,)";
    gltf += kEmbeddedPngBase64;
    gltf += R"(" } ],
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
  "buffers": [ { "byteLength": 68, "uri": "data:application/octet-stream;base64,)";
    gltf += kTriangleBufferBase64;
    gltf += R"(" } ]
})";
    return gltf;
}

class ScopedDir
{
  public:
    explicit ScopedDir(const char* name)
    {
        std::error_code ec;
        const std::filesystem::path base =
            std::filesystem::canonical(std::filesystem::temp_directory_path(), ec);
        m_path = (ec ? std::filesystem::temp_directory_path() : base) /
                 "miniengine_model_import_texture_tests" / name;
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

void WriteFixtureTo(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << BuildFixtureGltf();
}

size_t CountFilesIn(const std::filesystem::path& directory)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec))
    {
        return 0;
    }
    size_t count = 0;
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code fileEc;
        if (it->is_regular_file(fileEc) && !fileEc)
        {
            ++count;
        }
    }
    return count;
}

void ImportUnpacksEmbeddedTextures()
{
    ScopedDir scope("unpack");
    const std::filesystem::path source = scope.Path() / "source" / "fixture.gltf";
    WriteFixtureTo(source);

    const std::filesystem::path bundle = scope.Path() / "bundle";
    std::error_code ec;
    std::filesystem::create_directories(bundle, ec);
    const std::filesystem::path imported = ModelLoader::CopyModelWithSortedReferences(source, bundle);

    Require(std::filesystem::exists(imported, ec) && !ec, "import did not produce a model file");

    const std::filesystem::path textures = imported.parent_path() / "textures";
    Require(CountFilesIn(textures) == 1, "import did not unpack exactly one embedded texture");
}

void UnpackIsIdempotent()
{
    ScopedDir scope("idempotent");
    const std::filesystem::path model = scope.Path() / "fixture.gltf";
    WriteFixtureTo(model);

    GltfModelLoader::UnpackEmbeddedTextures(model);
    const std::filesystem::path textures = scope.Path() / "textures";
    Require(CountFilesIn(textures) == 1, "first unpack did not produce one texture");

    std::error_code ec;
    const std::filesystem::path unpacked = std::filesystem::directory_iterator(textures, ec)->path();
    const std::filesystem::file_time_type firstWrite = std::filesystem::last_write_time(unpacked, ec);

    GltfModelLoader::UnpackEmbeddedTextures(model);
    Require(CountFilesIn(textures) == 1, "second unpack duplicated the texture");
    Require(
        std::filesystem::last_write_time(unpacked, ec) == firstWrite,
        "second unpack rewrote an existing texture instead of skipping it");
}

void LoadReportsModelRelativeTexturePath()
{
    ScopedDir scope("relative");
    const std::filesystem::path source = scope.Path() / "source" / "fixture.gltf";
    WriteFixtureTo(source);

    const std::filesystem::path bundle = scope.Path() / "bundle";
    std::error_code ec;
    std::filesystem::create_directories(bundle, ec);
    const std::filesystem::path imported = ModelLoader::CopyModelWithSortedReferences(source, bundle);

    const LoadedModelData data = ModelLoader::LoadModel(imported.string());
    Require(!data.materials.empty(), "loaded model carried no material");

    const std::string& texturePath = data.materials[0].baseColorTexturePath;
    Require(!texturePath.empty(), "embedded base color texture resolved to nothing");
    Require(
        !std::filesystem::path(texturePath).is_absolute(),
        "embedded texture path is absolute; it must be relative to the model");
    Require(
        std::filesystem::exists(imported.parent_path() / texturePath, ec) && !ec,
        "texture path did not resolve against the model directory");
}

// The property Task 3 establishes: parsing a model writes nothing.
void LoadWritesNothing()
{
    ScopedDir scope("readonly");
    const std::filesystem::path source = scope.Path() / "source" / "fixture.gltf";
    WriteFixtureTo(source);

    const std::filesystem::path bundle = scope.Path() / "bundle";
    std::error_code ec;
    std::filesystem::create_directories(bundle, ec);
    const std::filesystem::path imported = ModelLoader::CopyModelWithSortedReferences(source, bundle);

    size_t before = 0;
    for (std::filesystem::recursive_directory_iterator it(bundle, ec), end; !ec && it != end; it.increment(ec))
    {
        ++before;
    }

    ModelLoader::LoadModel(imported.string());

    size_t after = 0;
    for (std::filesystem::recursive_directory_iterator it(bundle, ec), end; !ec && it != end; it.increment(ec))
    {
        ++after;
    }

    Require(before == after, "loading a model created or removed files in the bundle");
}

// A second import into the same folder used to keep the old model and report
// success. The copy now refuses, so conflicts are resolved before copying.
void CopyRefusesExistingTarget()
{
    ScopedDir scope("existing");
    const std::filesystem::path source = scope.Path() / "source" / "fixture.gltf";
    WriteFixtureTo(source);

    const std::filesystem::path bundle = scope.Path() / "bundle";
    std::error_code ec;
    std::filesystem::create_directories(bundle, ec);
    ModelLoader::CopyModelWithSortedReferences(source, bundle);

    bool threw = false;
    try
    {
        ModelLoader::CopyModelWithSortedReferences(source, bundle);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    Require(threw, "importing over an existing model reported success");
}

// A companion that cannot be copied used to leave its original URI in the
// imported glTF, pointing at a file the new folder does not have.
void CompanionCopyFailureFailsTheImport()
{
    ScopedDir scope("companion_failure");
    const std::filesystem::path source = scope.Path() / "source" / "model.gltf";
    std::error_code ec;
    std::filesystem::create_directories(source.parent_path(), ec);
    {
        std::ofstream(source.parent_path() / "bark.png", std::ios::binary) << "png";
        std::ofstream out(source, std::ios::binary);
        out << R"({ "asset": { "version": "2.0" }, "images": [ { "uri": "bark.png" } ] })";
    }

    // A file where the textures/ folder must go makes the companion copy fail.
    const std::filesystem::path bundle = scope.Path() / "bundle";
    std::filesystem::create_directories(bundle, ec);
    std::ofstream(bundle / "textures", std::ios::binary) << "blocker";

    bool threw = false;
    try
    {
        GltfModelLoader::CopyWithSortedReferences(source, bundle);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    Require(threw, "a failed companion copy still produced an imported glTF");
    Require(!std::filesystem::exists(bundle / "model.gltf", ec), "a glTF with a dangling URI was written");
}

// Two named materials, "Bark" then "Leaf", so their definition files can be
// swapped relative to the glTF's order.
std::string BuildTwoMaterialGltf()
{
    std::string gltf = BuildFixtureGltf();
    const std::string single =
        R"("materials": [ { "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } } ])";
    const size_t at = gltf.find(single);
    Require(at != std::string::npos, "fixture materials block changed; update the test");
    gltf.replace(at, single.size(), R"("materials": [ { "name": "Bark" }, { "name": "Leaf" } ])");
    return gltf;
}

void WriteDefinition(const std::filesystem::path& path, const char* materialName, float metallic)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "material:\n  name: " << materialName << "\n  pbr:\n    metallic_factor: " << metallic << "\n";
}

// Definitions are files named by index. When the glTF's materials are
// reordered outside the editor, each definition must follow its material by
// name instead of dressing whatever now sits at its index.
void MaterialDefinitionsFollowTheirMaterialByName()
{
    ScopedDir scope("definitions_by_name");
    const std::filesystem::path model = scope.Path() / "tree.gltf";
    {
        std::ofstream out(model, std::ios::binary);
        out << BuildTwoMaterialGltf();
    }

    // Saved when the order was Leaf, Bark.
    WriteDefinition(scope.Path() / "tree_0.material.yaml", "Leaf", 0.25f);
    WriteDefinition(scope.Path() / "tree_1.material.yaml", "Bark", 0.75f);

    const LoadedModelData data = ModelLoader::LoadModel(model.string());
    Require(data.materials.size() == 2, "fixture did not load two materials");
    Require(data.materials[0].name == "Bark" && data.materials[0].metallicFactor == 0.75f,
            "Bark did not get its own definition");
    Require(data.materials[1].name == "Leaf" && data.materials[1].metallicFactor == 0.25f,
            "Leaf did not get its own definition");
}

void MaterialDefinitionForAnotherMaterialIsSkipped()
{
    ScopedDir scope("definition_mismatch");
    const std::filesystem::path model = scope.Path() / "tree.gltf";
    {
        std::ofstream out(model, std::ios::binary);
        out << BuildTwoMaterialGltf();
    }
    WriteDefinition(scope.Path() / "tree_0.material.yaml", "Stone", 0.5f);

    const LoadedModelData data = ModelLoader::LoadModel(model.string());
    Require(data.materials[0].name == "Bark", "a definition for another material renamed Bark");
    Require(data.materials[0].metallicFactor != 0.5f, "a definition for another material was applied to Bark");
}

// The file dialog hands the engine UTF-8 paths as std::string. They must reach
// the disk as the same characters, which on Windows needs the UTF-8 process
// code page from miniengine_utf8.manifest.
void ImportFromNonAsciiPath()
{
#ifdef _WIN32
    Require(GetACP() == CP_UTF8, "process code page is not UTF-8; the manifest was not embedded");
#endif
    ScopedDir scope("non_ascii");

    // U+6A21 U+578B ("model") as a wide path, so the fixture lands at the real
    // characters regardless of how narrow strings are decoded.
    const std::filesystem::path sourceDir = scope.Path() / L"\u6a21\u578b";
    WriteFixtureTo(sourceDir / "fixture.gltf");

    // What the dialog returns: the same path as UTF-8 bytes.
    const std::string utf8SourcePath =
        scope.Path().string() + "\\\xE6\xA8\xA1\xE5\x9E\x8B"
                                "\\fixture.gltf";
    const std::filesystem::path bundle = scope.Path() / "bundle";
    std::error_code ec;
    std::filesystem::create_directories(bundle, ec);
    const std::filesystem::path imported =
        ModelLoader::CopyModelWithSortedReferences(std::filesystem::path(utf8SourcePath), bundle);

    const LoadedModelData data = ModelLoader::LoadModel(imported.string());
    Require(!data.submeshes.empty(), "model imported from a non-ASCII path did not load");
}

// A companion image is copied, never decoded, during import: an .exr that stb cannot read must not
// fail the unpack step, which only cares about embedded images.
void UnpackSkipsCompanionImages()
{
    ScopedDir scope("companion_exr");
    const std::filesystem::path model = scope.Path() / "fixture.gltf";
    std::string gltf = BuildFixtureGltf();
    const size_t start = gltf.find(R"("images": [ )");
    const std::string arrayEnd = " } ],";
    const size_t end = gltf.find(arrayEnd, start);
    Require(start != std::string::npos && end != std::string::npos, "fixture has no image array");
    gltf.replace(start, end + arrayEnd.size() - start, R"("images": [ { "uri": "radiance.exr" } ],)");
    {
        std::ofstream out(model, std::ios::binary | std::ios::trunc);
        out << gltf;
    }
    {
        std::ofstream out(scope.Path() / "radiance.exr", std::ios::binary | std::ios::trunc);
        out << "v/1 not something stb can decode";
    }

    GltfModelLoader::UnpackEmbeddedTextures(model);
    Require(CountFilesIn(scope.Path() / "textures") == 0, "a companion image must not be unpacked");
}
}

int main()
{
    try
    {
        ImportUnpacksEmbeddedTextures();
        UnpackIsIdempotent();
        LoadReportsModelRelativeTexturePath();
        LoadWritesNothing();
        CopyRefusesExistingTarget();
        CompanionCopyFailureFailsTheImport();
        MaterialDefinitionsFollowTheirMaterialByName();
        MaterialDefinitionForAnotherMaterialIsSkipped();
        ImportFromNonAsciiPath();
        UnpackSkipsCompanionImages();

        std::cout << "model import texture tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "model import texture tests failed: " << error.what() << '\n';
        return 1;
    }
}
