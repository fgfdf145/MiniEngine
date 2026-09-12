#include <engine/asset/gltf_model_loader.h>
#include <engine/asset/model_loader.h>

#include <filesystem>
#include <fstream>
#include <iostream>
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
}

int main()
{
    try
    {
        ImportUnpacksEmbeddedTextures();
        UnpackIsIdempotent();

        std::cout << "model import texture tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "model import texture tests failed: " << error.what() << '\n';
        return 1;
    }
}
