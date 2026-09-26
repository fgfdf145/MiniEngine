#include <engine/asset/gltf_model_loader.h>
#include <engine/asset/model_loader.h>
#include <engine/asset/texture_loader.h>

#include <ktx.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

// main() stays in the global namespace; everything it drives lives in me::.
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

struct ScopedFixtureDirectory
{
    ScopedFixtureDirectory()
    {
        std::random_device randomDevice;
        path = std::filesystem::temp_directory_path() /
               ("miniengine_ktx2_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                std::to_string(randomDevice()));
        std::filesystem::create_directories(path);
    }

    ~ScopedFixtureDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

constexpr uint32_t kWidth = 8;
constexpr uint32_t kHeight = 8;

// Four flat 4 x 4 blocks (red, green, blue, grey with half alpha): a picture block compression
// keeps close to exact, and one whose rows and columns show if the image comes back flipped.
std::vector<uint8_t> SourcePixels()
{
    std::vector<uint8_t> pixels(kWidth * kHeight * 4);
    for (uint32_t y = 0; y < kHeight; ++y)
    {
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            const uint32_t block = (y / 4) * 2 + x / 4;
            const uint8_t colors[4][4] = {{220, 30, 30, 255}, {30, 200, 40, 255}, {40, 50, 210, 255}, {128, 128, 128, 128}};
            std::memcpy(&pixels[(y * kWidth + x) * 4], colors[block], 4);
        }
    }
    return pixels;
}

enum class Encoding
{
    Rgba8,
    Etc1s,
    Uastc,
};

// The source pixels as a KTX2 file written by libktx.
std::vector<uint8_t> WriteKtx2(Encoding encoding)
{
    ktxTextureCreateInfo info{};
    info.vkFormat = 37; // VK_FORMAT_R8G8B8A8_UNORM
    info.baseWidth = kWidth;
    info.baseHeight = kHeight;
    info.baseDepth = 1;
    info.numDimensions = 2;
    info.numLevels = 1;
    info.numLayers = 1;
    info.numFaces = 1;
    info.isArray = KTX_FALSE;
    info.generateMipmaps = KTX_FALSE;
    ktxTexture2* texture = nullptr;
    Require(ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture) == KTX_SUCCESS, "libktx creates a texture");
    const std::vector<uint8_t> pixels = SourcePixels();
    Require(ktxTexture_SetImageFromMemory(ktxTexture(texture), 0, 0, 0, pixels.data(), pixels.size()) == KTX_SUCCESS, "libktx takes the pixels");
    if (encoding != Encoding::Rgba8)
    {
        ktxBasisParams params{};
        params.structSize = sizeof(params);
        params.uastc = encoding == Encoding::Uastc ? KTX_TRUE : KTX_FALSE;
        params.threadCount = 1;
        params.qualityLevel = 255;
        Require(ktxTexture2_CompressBasisEx(texture, &params) == KTX_SUCCESS, "libktx compresses to Basis Universal");
    }
    ktx_uint8_t* bytes = nullptr;
    ktx_size_t size = 0;
    Require(ktxTexture_WriteToMemory(ktxTexture(texture), &bytes, &size) == KTX_SUCCESS, "libktx writes the file");
    std::vector<uint8_t> file(bytes, bytes + size);
    std::free(bytes);
    ktxTexture_Destroy(ktxTexture(texture));
    return file;
}

void WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Largest per-channel difference from the source pixels.
int MaxDifference(const TextureData& texture)
{
    Require(texture.width == static_cast<int>(kWidth) && texture.height == static_cast<int>(kHeight) && texture.pixels.size() == kWidth * kHeight * 4,
            "the texture keeps its size");
    const std::vector<uint8_t> source = SourcePixels();
    int difference = 0;
    for (size_t index = 0; index < source.size(); ++index)
    {
        difference = std::max(difference, std::abs(static_cast<int>(texture.pixels[index]) - static_cast<int>(source[index])));
    }
    return difference;
}

// An uncompressed RGBA8 KTX2 loads with its exact pixels; Basis Universal ETC1S and UASTC files
// transcode close to them, the right way up.
void Ktx2FilesLoad()
{
    const ScopedFixtureDirectory directory;
    const struct
    {
        Encoding encoding;
        const char* name;
        int tolerance;
    } cases[] = {{Encoding::Rgba8, "rgba8", 0}, {Encoding::Uastc, "uastc", 8}, {Encoding::Etc1s, "etc1s", 24}};
    for (const auto& testCase : cases)
    {
        const std::filesystem::path path = directory.path / (std::string(testCase.name) + ".ktx2");
        const std::vector<uint8_t> bytes = WriteKtx2(testCase.encoding);
        WriteBytes(path, bytes);
        Require(TextureLoader::IsKtx2(bytes.data(), bytes.size()), std::string(testCase.name) + ": the file identifier is recognised");
        const int difference = MaxDifference(TextureLoader::LoadRGBA8(path.string()));
        Require(difference <= testCase.tolerance,
                std::string(testCase.name) + ": decodes within " + std::to_string(testCase.tolerance) + " of the source, not " + std::to_string(difference));
    }
    const uint8_t png[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    Require(!TextureLoader::IsKtx2(png, sizeof(png)), "a PNG is not taken for KTX2");
}

// KHR_texture_basisu: a texture's KTX2 source wins over its PNG fallback, and a KTX2 image
// embedded in a .glb unpacks to a PNG with its pixels.
void GltfKtx2Textures()
{
    const ScopedFixtureDirectory directory;
    const std::vector<uint8_t> ktx2 = WriteKtx2(Encoding::Uastc);
    WriteBytes(directory.path / "color.ktx2", ktx2);

    std::vector<uint8_t> bin(36, 0);
    const float positions[9] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    std::memcpy(bin.data(), positions, sizeof(positions));
    const size_t imageOffset = bin.size();
    bin.insert(bin.end(), ktx2.begin(), ktx2.end());
    while (bin.size() % 4 != 0)
    {
        bin.push_back(0);
    }

    const auto json = [&](bool embedded)
    {
        const std::string images = embedded ? R"([{ "name": "embedded", "bufferView": 1, "mimeType": "image/ktx2" }])"
                                            : R"([{ "uri": "fallback.png" }, { "uri": "color.ktx2" }])";
        const std::string texture = embedded ? R"({ "extensions": { "KHR_texture_basisu": { "source": 0 } } })"
                                             : R"({ "source": 0, "extensions": { "KHR_texture_basisu": { "source": 1 } } })";
        return std::string(R"({ "asset": { "version": "2.0" },
          "extensionsUsed": ["KHR_texture_basisu"], "extensionsRequired": ["KHR_texture_basisu"],
          "buffers": [{ )") +
               (embedded ? "" : R"("uri": "model.bin", )") + R"("byteLength": )" + std::to_string(bin.size()) + R"( }],
          "bufferViews": [{ "buffer": 0, "byteOffset": 0, "byteLength": 36 },
                          { "buffer": 0, "byteOffset": )" +
               std::to_string(imageOffset) + R"(, "byteLength": )" + std::to_string(ktx2.size()) + R"( }],
          "accessors": [{ "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0] }],
          "images": )" +
               images +
               R"(,
          "textures": [)" +
               texture + R"(],
          "materials": [{ "pbrMetallicRoughness": { "baseColorTexture": { "index": 0 } } }],
          "meshes": [{ "primitives": [{ "attributes": { "POSITION": 0 }, "material": 0 }] }],
          "nodes": [{ "mesh": 0 }], "scenes": [{ "nodes": [0] }], "scene": 0 })";
    };

    WriteBytes(directory.path / "model.bin", bin);
    WriteBytes(directory.path / "fallback.png", {});
    std::ofstream(directory.path / "external.gltf") << json(false);
    const LoadedModelData external = ModelLoader::LoadModel((directory.path / "external.gltf").string());
    Require(external.materials.at(0).baseColorTexturePath == "color.ktx2",
            "the KTX2 source wins over the fallback, not '" + external.materials.at(0).baseColorTexturePath + "'");

    std::string glbJson = json(true);
    while (glbJson.size() % 4 != 0)
    {
        glbJson.push_back(' ');
    }
    std::vector<uint8_t> glb;
    const auto append32 = [&glb](uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            glb.push_back(static_cast<uint8_t>(value >> shift));
        }
    };
    append32(0x46546C67u);
    append32(2u);
    append32(static_cast<uint32_t>(12 + 8 + glbJson.size() + 8 + bin.size()));
    append32(static_cast<uint32_t>(glbJson.size()));
    append32(0x4E4F534Au);
    glb.insert(glb.end(), glbJson.begin(), glbJson.end());
    append32(static_cast<uint32_t>(bin.size()));
    append32(0x004E4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());
    const std::filesystem::path glbPath = directory.path / "embedded.glb";
    WriteBytes(glbPath, glb);

    GltfModelLoader::UnpackEmbeddedTextures(glbPath);
    const LoadedModelData embedded = ModelLoader::LoadModel(glbPath.string());
    const std::string& unpacked = embedded.materials.at(0).baseColorTexturePath;
    Require(unpacked.ends_with(".png"), "an embedded KTX2 image unpacks to a PNG, not '" + unpacked + "'");
    const int difference = MaxDifference(TextureLoader::LoadRGBA8((directory.path / unpacked).string()));
    Require(difference <= 8, "the unpacked PNG keeps the KTX2 image's pixels, off by " + std::to_string(difference));
}
}

int main()
{
    try
    {
        Ktx2FilesLoad();
        GltfKtx2Textures();
    }
    catch (const std::exception& error)
    {
        std::cerr << "KTX2 texture tests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "KTX2 texture tests passed\n";
    return 0;
}
