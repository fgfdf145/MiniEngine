#include <engine/asset/ac_car_data.h>
#include <engine/asset/acd_archive.h>
#include <engine/asset/dds_decoder.h>
#include <engine/asset/kn5_importer.h>
#include <engine/asset/kn5_reader.h>
#include <engine/asset/model_loader.h>

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <iostream>
#include <limits>
#include <map>
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

void RequireNear(float actual, float expected, float tolerance, const std::string& what)
{
    Require(std::abs(actual - expected) <= tolerance,
            what + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}

template <typename Function>
void RequireThrows(Function&& function, const std::string& what)
{
    try
    {
        function();
    }
    catch (const std::exception&)
    {
        return;
    }
    throw std::runtime_error(what + " did not throw");
}

class ScopedDirectory
{
  public:
    ScopedDirectory()
    {
        std::random_device randomDevice;
        m_path = std::filesystem::temp_directory_path() /
                 ("miniengine_kn5_import_" +
                  std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()) + "_" +
                  std::to_string(randomDevice()));
        std::filesystem::create_directories(m_path);
    }

    ~ScopedDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    const std::filesystem::path& Path() const
    {
        return m_path;
    }

  private:
    std::filesystem::path m_path;
};

class ByteWriter
{
  public:
    void U8(std::uint8_t value)
    {
        m_bytes.push_back(value);
    }

    void U16(std::uint16_t value)
    {
        Raw(&value, 2);
    }

    void U32(std::uint32_t value)
    {
        Raw(&value, 4);
    }

    void I32(std::int32_t value)
    {
        Raw(&value, 4);
    }

    void F32(float value)
    {
        Raw(&value, 4);
    }

    void String(const std::string& value)
    {
        U32(static_cast<std::uint32_t>(value.size()));
        Raw(value.data(), value.size());
    }

    void Blob(const std::vector<std::uint8_t>& value)
    {
        U32(static_cast<std::uint32_t>(value.size()));
        Raw(value.data(), value.size());
    }

    void Raw(const void* data, size_t size)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        m_bytes.insert(m_bytes.end(), bytes, bytes + size);
    }

    std::vector<std::uint8_t>& Bytes()
    {
        return m_bytes;
    }

  private:
    std::vector<std::uint8_t> m_bytes;
};

// ---- DDS fixtures ------------------------------------------------------------------------------

std::vector<std::uint8_t> DdsHeader(int width, int height, std::uint32_t pixelFlags, std::uint32_t fourCc,
                                    std::uint32_t bitCount, std::array<std::uint32_t, 4> masks)
{
    ByteWriter writer;
    writer.Raw("DDS ", 4);
    writer.U32(124);
    writer.U32(0x1 | 0x2 | 0x4 | 0x1000); // caps, height, width, pixel format
    writer.U32(static_cast<std::uint32_t>(height));
    writer.U32(static_cast<std::uint32_t>(width));
    writer.U32(0);
    writer.U32(0);
    writer.U32(1);
    for (int reserved = 0; reserved < 11; ++reserved)
    {
        writer.U32(0);
    }
    writer.U32(32);
    writer.U32(pixelFlags);
    writer.U32(fourCc);
    writer.U32(bitCount);
    for (std::uint32_t mask : masks)
    {
        writer.U32(mask);
    }
    writer.U32(0x1000);
    for (int caps = 0; caps < 4; ++caps)
    {
        writer.U32(0);
    }
    return writer.Bytes();
}

// A8R8G8B8 from RGBA texels.
std::vector<std::uint8_t> DdsBgra(int width, int height, const std::vector<std::array<std::uint8_t, 4>>& texels)
{
    std::vector<std::uint8_t> dds =
        DdsHeader(width, height, 0x40 | 0x1, 0, 32, {0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u});
    for (const auto& texel : texels)
    {
        dds.push_back(texel[2]);
        dds.push_back(texel[1]);
        dds.push_back(texel[0]);
        dds.push_back(texel[3]);
    }
    return dds;
}

std::vector<std::uint8_t> DdsFlat(int size, std::array<std::uint8_t, 4> color)
{
    return DdsBgra(size, size, std::vector<std::array<std::uint8_t, 4>>(static_cast<size_t>(size * size), color));
}

constexpr std::uint32_t FourCc(const char (&text)[5])
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(text[0])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(text[3])) << 24);
}

std::vector<std::uint8_t> DdsBlocks(int width, int height, std::uint32_t fourCc, const std::vector<std::uint8_t>& blocks)
{
    std::vector<std::uint8_t> dds = DdsHeader(width, height, 0x4, fourCc, 0, {0, 0, 0, 0});
    dds.insert(dds.end(), blocks.begin(), blocks.end());
    return dds;
}

void DdsDecodesBc1()
{
    // c0 pure red, c1 pure blue; texel 0 takes c0, texel 1 c1, texel 2 two thirds red, texel 3 two
    // thirds blue; the rest c0.
    const std::vector<std::uint8_t> block{0x00, 0xF8, 0x1F, 0x00, 0b11100100, 0, 0, 0};
    const std::vector<std::uint8_t> dds = DdsBlocks(4, 4, FourCc("DXT1"), block);
    const TextureData image = DdsDecoder::Decode(dds.data(), dds.size(), "bc1");
    Require(image.width == 4 && image.height == 4, "BC1 size");
    const auto texel = [&](int index)
    {
        return std::array<int, 4>{image.pixels[index * 4], image.pixels[index * 4 + 1], image.pixels[index * 4 + 2],
                                  image.pixels[index * 4 + 3]};
    };
    Require(texel(0) == std::array<int, 4>{255, 0, 0, 255}, "BC1 endpoint 0");
    Require(texel(1) == std::array<int, 4>{0, 0, 255, 255}, "BC1 endpoint 1");
    Require(texel(2) == std::array<int, 4>{170, 0, 85, 255}, "BC1 interpolant 2");
    Require(texel(3) == std::array<int, 4>{85, 0, 170, 255}, "BC1 interpolant 3");
    Require(texel(4) == std::array<int, 4>{255, 0, 0, 255}, "BC1 index 0 elsewhere");

    // c0 <= c1: three colours and a transparent black.
    const std::vector<std::uint8_t> punchThrough{0x1F, 0x00, 0x00, 0xF8, 0b11, 0, 0, 0};
    const std::vector<std::uint8_t> ddsAlpha = DdsBlocks(4, 4, FourCc("DXT1"), punchThrough);
    const TextureData alpha = DdsDecoder::Decode(ddsAlpha.data(), ddsAlpha.size(), "bc1a");
    Require(alpha.pixels[3] == 0 && alpha.pixels[0] == 0, "BC1 index 3 in three-colour mode is transparent black");
}

void DdsDecodesBc3AndBc4()
{
    // Alpha endpoints 255 and 0, every index 1 (the second endpoint): alpha 0. Colour all white.
    std::vector<std::uint8_t> block{255, 0, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24};
    block.insert(block.end(), {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0});
    const std::vector<std::uint8_t> bc3 = DdsBlocks(4, 4, FourCc("DXT5"), block);
    const TextureData image = DdsDecoder::Decode(bc3.data(), bc3.size(), "bc3");
    Require(image.pixels[0] == 255 && image.pixels[3] == 0, "BC3 alpha takes the index's endpoint");

    const std::vector<std::uint8_t> bc4Block{200, 10, 0, 0, 0, 0, 0, 0};
    const std::vector<std::uint8_t> bc4 = DdsBlocks(4, 4, FourCc("ATI1"), bc4Block);
    const TextureData grey = DdsDecoder::Decode(bc4.data(), bc4.size(), "bc4");
    Require(grey.pixels[0] == 200 && grey.pixels[1] == 200 && grey.pixels[2] == 200 && grey.pixels[3] == 255,
            "BC4 decodes to opaque grey");

    // A non-multiple-of-four size crops the last blocks.
    const std::vector<std::uint8_t> odd = DdsBlocks(5, 3, FourCc("ATI1"), std::vector<std::uint8_t>(16, 0));
    const TextureData cropped = DdsDecoder::Decode(odd.data(), odd.size(), "odd");
    Require(cropped.width == 5 && cropped.height == 3 && cropped.pixels.size() == 5 * 3 * 4, "partial blocks crop");
}

void DdsDecodesMaskedFormats()
{
    const std::vector<std::uint8_t> bgra = DdsBgra(1, 1, {{10, 20, 30, 40}});
    const TextureData colour = DdsDecoder::Decode(bgra.data(), bgra.size(), "bgra");
    Require(colour.pixels == std::vector<std::uint8_t>{10, 20, 30, 40}, "A8R8G8B8 decodes to RGBA");

    // L8, luminance only: grey and opaque.
    std::vector<std::uint8_t> luminance = DdsHeader(1, 1, 0x20000, 0, 8, {0xFF, 0, 0, 0});
    luminance.push_back(77);
    const TextureData grey = DdsDecoder::Decode(luminance.data(), luminance.size(), "l8");
    Require(grey.pixels == std::vector<std::uint8_t>{77, 77, 77, 255}, "L8 decodes to opaque grey");

    // R5G6B5: white stays white after expansion to 8 bits.
    std::vector<std::uint8_t> rgb565 = DdsHeader(1, 1, 0x40, 0, 16, {0xF800, 0x07E0, 0x001F, 0});
    rgb565.push_back(0xFF);
    rgb565.push_back(0xFF);
    const TextureData white = DdsDecoder::Decode(rgb565.data(), rgb565.size(), "565");
    Require(white.pixels == std::vector<std::uint8_t>{255, 255, 255, 255}, "R5G6B5 expands to full range");
}

void DdsRejectsBadInput()
{
    std::vector<std::uint8_t> truncated = DdsBlocks(8, 8, FourCc("DXT1"), std::vector<std::uint8_t>(8, 0));
    RequireThrows([&]
                  {
                      DdsDecoder::Decode(truncated.data(), truncated.size(), "short");
                  },
                  "a truncated DDS");
    std::vector<std::uint8_t> unknown = DdsBlocks(4, 4, FourCc("ZZZZ"), std::vector<std::uint8_t>(16, 0));
    RequireThrows([&]
                  {
                      DdsDecoder::Decode(unknown.data(), unknown.size(), "zzzz");
                  },
                  "an unknown FourCC");
    const std::uint8_t notDds[4] = {'P', 'N', 'G', 0};
    Require(!DdsDecoder::IsDds(notDds, sizeof(notDds)), "a PNG is not a DDS");
}

// ---- kn5 fixture -------------------------------------------------------------------------------

struct FixtureMaterial
{
    std::string name;
    std::string shader;
    bool blend = false;
    bool tested = false;
    std::vector<std::pair<std::string, float>> properties;
    std::vector<std::pair<std::string, std::string>> textures;
    // Three-component values (valueC), for the parameters that are colours.
    std::map<std::string, std::array<float, 3>> vectors;
};

void WriteMaterial(ByteWriter& writer, const FixtureMaterial& material)
{
    writer.String(material.name);
    writer.String(material.shader);
    writer.U8(material.blend ? 1 : 0);
    writer.U8(material.tested ? 1 : 0);
    writer.I32(1); // depthMode
    writer.U32(static_cast<std::uint32_t>(material.properties.size()));
    for (const auto& [name, value] : material.properties)
    {
        writer.String(name);
        writer.F32(value);
        writer.F32(0.0f); // valueB
        writer.F32(0.0f);
        const auto vector = material.vectors.find(name);
        for (size_t channel = 0; channel < 3; ++channel)
        {
            writer.F32(vector == material.vectors.end() ? 0.0f : vector->second[channel]);
        }
        for (int unused = 0; unused < 4; ++unused) // valueD
        {
            writer.F32(0.0f);
        }
    }
    writer.U32(static_cast<std::uint32_t>(material.textures.size()));
    for (const auto& [slot, texture] : material.textures)
    {
        writer.String(slot);
        writer.U32(0);
        writer.String(texture);
    }
}

void BeginNode(ByteWriter& writer, int type, const std::string& name, std::uint32_t children)
{
    writer.I32(type);
    writer.String(name);
    writer.U32(children);
    writer.U8(1);
}

void WriteDummy(ByteWriter& writer, const std::string& name, std::uint32_t children, const std::array<float, 16>& matrix)
{
    BeginNode(writer, 1, name, children);
    for (float value : matrix)
    {
        writer.F32(value);
    }
}

// One triangle: (x, 0, 0), (0, 1, 0), (0, 0, 1), offset by `x` so each mesh is recognisable.
void WriteMesh(ByteWriter& writer, const std::string& name, std::uint32_t material, float x, bool skinned = false,
               std::uint32_t children = 0, bool renderable = true, float lodIn = 0.0f)
{
    BeginNode(writer, skinned ? 3 : 2, name, children);
    writer.U8(1); // castShadows
    writer.U8(1); // visible
    writer.U8(0); // transparent
    if (skinned)
    {
        writer.U32(1);
        writer.String("BONE");
        for (int value = 0; value < 16; ++value)
        {
            writer.F32(value % 5 == 0 ? 1.0f : 0.0f);
        }
    }
    const std::array<std::array<float, 3>, 3> positions{{{x, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    writer.U32(3);
    for (const auto& position : positions)
    {
        for (float value : position)
        {
            writer.F32(value);
        }
        // Normal (unnormalised on purpose), uv, tangent.
        writer.F32(0.0f);
        writer.F32(2.0f);
        writer.F32(0.0f);
        writer.F32(position[0]);
        writer.F32(position[1]);
        writer.F32(1.0f);
        writer.F32(0.0f);
        writer.F32(0.0f);
        if (skinned)
        {
            for (int value = 0; value < 8; ++value)
            {
                writer.F32(value == 0 ? 1.0f : 0.0f);
            }
        }
    }
    writer.U32(3);
    writer.U16(0);
    writer.U16(1);
    writer.U16(2);
    writer.U32(material);
    writer.U32(0);    // layer
    writer.F32(lodIn);
    writer.F32(1e6f); // lodOut
    if (!skinned)
    {
        writer.F32(0.0f);
        writer.F32(0.0f);
        writer.F32(0.0f);
        writer.F32(1.0f);
        writer.U8(renderable ? 1 : 0);
    }
}

constexpr std::array<float, 16> kIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

std::vector<std::uint8_t> BuildCarKn5(int version)
{
    ByteWriter writer;
    writer.Raw("sc6969", 6);
    writer.U32(static_cast<std::uint32_t>(version));
    if (version > 5)
    {
        writer.U32(0);
    }

    // The diffuse is the shared grey template, with the fully transparent alpha several cars ship
    // on opaque maps. The paint is the flat detail map.
    std::vector<std::array<std::uint8_t, 4>> mapTexels;
    for (int texel = 0; texel < 16; ++texel)
    {
        mapTexels.push_back({static_cast<std::uint8_t>(texel * 17), 255, 136, 255});
    }
    std::vector<std::array<std::uint8_t, 4>> leafTexels;
    for (int texel = 0; texel < 16; ++texel)
    {
        leafTexels.push_back({30, 120, 30, static_cast<std::uint8_t>(texel < 8 ? 0 : 255)});
    }
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>> textures{
        {"Skin_00.dds", DdsFlat(4, {174, 174, 174, 0})},
        {"metal_detail.dds", DdsFlat(4, {148, 148, 148, 255})},
        {"car_MAP.dds", DdsBgra(4, 4, mapTexels)},
        {"leaf.dds", DdsBgra(4, 4, leafTexels)},
        {"INT_DEcals.dds", DdsFlat(4, {10, 10, 200, 255})},
        {"Car_Damage_NM.dds", DdsFlat(4, {128, 128, 255, 255})},
        {"stub.dds", std::vector<std::uint8_t>(16, 0)},
    };
    writer.U32(static_cast<std::uint32_t>(textures.size()));
    for (const auto& [name, data] : textures)
    {
        writer.U32(1);
        writer.String(name);
        writer.Blob(data);
    }

    const std::vector<FixtureMaterial> materials{
        {"EXT_Carpaint", "ksPerPixelMultiMap_damage_dirt", false, false, {{"ksSpecular", 0.6f}, {"ksSpecularEXP", 80.0f}, {"useDetail", 1.0f}, {"detailUVMultiplier", 40.0f}, {"fresnelC", 0.07f}, {"fresnelEXP", 3.5f}, {"fresnelMaxLevel", 0.6f}, {"isAdditive", 2.0f}, {"sunSpecular", 10.0f}, {"sunSpecularEXP", 2000.0f}}, {{"txDiffuse", "Skin_00.dds"}, {"txDetail", "metal_detail.dds"}, {"txMaps", "car_MAP.dds"}, {"txNormal", "Car_Damage_NM.dds"}}},
        {"Leaves", "ksTree", false, true, {{"ksSpecular", 0.0f}, {"ksAlphaRef", 0.0f}}, {{"txDiffuse", "leaf.dds"}}},
        {"Glass", "ksPerPixel", true, false, {{"ksSpecular", 0.5f}, {"ksSpecularEXP", 200.0f}}, {{"txDiffuse", "INT_Decals.dds"}}},
        {"Lamp", "ksPerPixel", false, false, {{"ksEmissive", 3.0f}}, {{"txDiffuse", "stub.dds"}}},
    };
    writer.U32(static_cast<std::uint32_t>(materials.size()));
    for (const FixtureMaterial& material : materials)
    {
        WriteMaterial(writer, material);
    }

    std::array<float, 16> wheel = kIdentity;
    wheel[12] = 0.7f;
    wheel[13] = 0.3f;
    wheel[14] = 1.2f;
    std::array<float, 16> broken = kIdentity;
    broken[0] = std::numeric_limits<float>::quiet_NaN();

    WriteDummy(writer, "ROOT", 8, kIdentity);
    WriteMesh(writer, "BODY", 0, 1.0f);
    WriteMesh(writer, "BODY_DAMAGE", 0, 2.0f);
    WriteDummy(writer, "WHEEL_LF", 2, wheel);
    WriteMesh(writer, "RIM_LF", 0, 0.0f);
    WriteMesh(writer, "RIM_BLUR_LF", 0, 0.0f);
    WriteMesh(writer, "STEER_HR", 2, 3.0f);
    WriteMesh(writer, "STEER_LR", 2, 4.0f);
    WriteMesh(writer, "WHEEL_LR", 3, 5.0f);
    WriteMesh(writer, "GEAR", 1, 6.0f, true);
    WriteDummy(writer, "NAN_DUMMY", 0, broken);
    return writer.Bytes();
}

void WriteFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// A car folder the way Assetto Corsa lays it out: the kn5 and skins/<livery>/ beside it.
std::filesystem::path WriteCarFolder(const std::filesystem::path& root, int version = 6)
{
    const std::filesystem::path car = root / "ks_fixture";
    const std::filesystem::path kn5 = car / "fixture_lod_a.kn5";
    WriteFile(kn5, BuildCarKn5(version));
    // Upper-case file name on purpose: the game matches liveries ignoring case.
    WriteFile(car / "skins" / "00_soul_red" / "METAL_DETAIL.dds", DdsFlat(4, {126, 1, 0, 69}));
    std::vector<std::array<std::uint8_t, 4>> flakes;
    for (int texel = 0; texel < 16; ++texel)
    {
        flakes.push_back({213, 210, 208, static_cast<std::uint8_t>(texel % 2 == 0 ? 30 : 110)});
    }
    WriteFile(car / "skins" / "01_arctic_white" / "metal_detail.dds", DdsBgra(4, 4, flakes));
    return kn5;
}

const ModelSubmeshData& FindSubmesh(const LoadedModelData& model, const char* nodeName)
{
    const std::string prefix = std::string(nodeName) + "/";
    for (const ModelSubmeshData& submesh : model.submeshes)
    {
        if (submesh.name.rfind(prefix, 0) == 0)
        {
            return submesh;
        }
    }
    throw std::runtime_error("no submesh for node '" + std::string(nodeName) + "'");
}

bool HasSubmesh(const LoadedModelData& model, const std::string& nodeName)
{
    return std::any_of(model.submeshes.begin(), model.submeshes.end(), [&](const ModelSubmeshData& submesh)
                       {
                           return submesh.name.rfind(nodeName + "/", 0) == 0;
                       });
}

int PngChannels(const std::filesystem::path& path)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    Require(stbi_info(path.string().c_str(), &width, &height, &channels) != 0, "not a readable image: " + path.string());
    return channels;
}

// The texels of a PNG the import wrote, as RGBA.
std::vector<std::uint8_t> ReadPngRgba(const std::string& path, int& width, int& height)
{
    int channels = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    Require(pixels != nullptr, "readable png " + path);
    std::vector<std::uint8_t> rgba(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    stbi_image_free(pixels);
    return rgba;
}

void ReaderParsesTheContainer()
{
    for (int version : {5, 6})
    {
        const std::vector<std::uint8_t> bytes = BuildCarKn5(version);
        const Kn5Model model = Kn5Reader::Parse(bytes, "fixture");
        Require(model.version == static_cast<std::uint32_t>(version), "version");
        Require(model.textures.size() == 7 && model.materials.size() == 4, "texture and material tables");
        Require(!Kn5Reader::IsEncrypted(bytes), "a plain kn5 is not encrypted");
        Require(model.materials[1].alphaTested && model.materials[2].alphaBlend, "alpha flags");
        RequireNear(model.materials[0].Property("sunSpecularEXP", 0.0f), 2000.0f, 0.0f, "material property");
        Require(model.root.name == "ROOT" && model.root.children.size() == 8, "root and its children");
        // The skinned node is followed by another node: reading past it would have desynced.
        const Kn5Node& gear = model.root.children[6];
        Require(gear.type == Kn5NodeType::Skinned && gear.vertices.size() == 3, "skinned node geometry");
        Require(model.root.children[7].name == "NAN_DUMMY", "the node after a skinned one parses");
        Require(model.root.children[2].children.size() == 2, "nested children");
        RequireNear(model.root.children[2].matrix[12], 0.7f, 0.0f, "dummy matrix");
    }

    const Kn5Model counted = Kn5Reader::Parse(BuildCarKn5(6), "fixture", Kn5ReadScope::NoGeometry);
    Require(counted.root.children[0].vertices.empty() && counted.root.children[0].vertexCount == 3 &&
                counted.root.children[0].triangleCount == 1,
            "a geometry-less read keeps the counts");
    const Kn5Model tables = Kn5Reader::Parse(BuildCarKn5(6), "fixture", Kn5ReadScope::Tables);
    Require(tables.textures.size() == 7 && tables.textures[0].data.empty() && tables.textures[0].size > 128,
            "a tables read keeps names and sizes, not data");
    Require(tables.materials.size() == 4 && tables.root.children.empty(), "a tables read stops after the materials");
    const Kn5Model textures = Kn5Reader::Parse(BuildCarKn5(6), "fixture", Kn5ReadScope::Textures);
    Require(textures.textures[0].data.size() == textures.textures[0].size, "a textures read keeps the data");

    std::vector<std::uint8_t> truncated = BuildCarKn5(6);
    truncated.resize(truncated.size() - 10);
    RequireThrows([&]
                  {
                      Kn5Reader::Parse(truncated, "truncated");
                  },
                  "a truncated kn5");
    RequireThrows([&]
                  {
                      Kn5Reader::Parse({'n', 'o', 'p', 'e', 0, 0, 0, 0}, "nope");
                  },
                  "a file that is not a kn5");
}

void RulesMatchTheConverter()
{
    Require(Kn5Importer::IsRuntimeVariant("WHEEL_BLUR_LF") && Kn5Importer::IsRuntimeVariant("door_damage"),
            "blur and damage are runtime variants");
    Require(!Kn5Importer::IsRuntimeVariant("WHEEL_LF"), "a wheel is not a variant");
    for (const char* marker : {"AC_START_0", "AC_PIT_12", "AC_HOTLAP_START_0", "AC_TIME_0_L", "ac_time_2_r"})
    {
        Require(Kn5Importer::IsTrackMarker(marker), std::string(marker) + " is a track marker");
    }
    for (const char* object : {"AC_POBJECT_011", "AC_CREW_0_LODA1", "AC_PITLANE_WALL", "AC_TIME_0", "START_LINE"})
    {
        Require(!Kn5Importer::IsTrackMarker(object), std::string(object) + " is drawn");
    }

    const std::set<std::string> twins =
        Kn5Importer::LowResTwins({"COCKPIT_HR", "cockpit_lr", "WHEEL_LR", "SUSP_LR", "STEER_HR", "STEER_LR"});
    Require(twins == std::set<std::string>{"cockpit_lr", "STEER_LR"}, "only _LR names with an _HR twin are low-res");

    Require(Kn5Importer::IsPhysicsMeshName("1ROAD") && Kn5Importer::IsPhysicsMeshName("20ASPH-SPA_BLACK_004"),
            "a name that starts with a digit is a physics mesh");
    Require(!Kn5Importer::IsPhysicsMeshName("cameraface1_KSLAYER3") && !Kn5Importer::IsPhysicsMeshName("AC_START_0") &&
                !Kn5Importer::IsPhysicsMeshName(""),
            "the other meshes the game never draws are not");

    const std::vector<Kn5Surface> surfaces = Kn5Importer::ParseSurfaces(
        "[SURFACE_0]\nKEY=ASPH-SPA_BLACK\nFRICTION=0.98\nDAMPING=0\n\n"
        "[SURFACE_1]\nKEY=ASPH\nFRICTION=0.5\n\n"
        "[SURFACE_2]\nKEY=GRASS\nFRICTION=nonsense\n\n"
        "[SURFACE_3]\nFRICTION=0.3\n\n"
        "[OTHER]\nKEY=IGNORED\nFRICTION=0.1\n");
    Require(surfaces.size() == 3 && surfaces[0].key == "ASPH-SPA_BLACK", "a KEY makes a surface; a section without one, or not a SURFACE, does not");
    RequireNear(surfaces[0].friction, 0.98f, 1e-6f, "FRICTION is read");
    RequireNear(surfaces[2].friction, kUnknownSurfaceFriction, 1e-6f, "a malformed FRICTION is the unknown surface's");
    Kn5Surface match = Kn5Importer::MatchSurface(surfaces, "20ASPH-SPA_BLACK_004");
    Require(match.key == "ASPH-SPA_BLACK", "the longest KEY the name starts with, leading digits dropped");
    Require(Kn5Importer::MatchSurface(surfaces, "07asph-spa_black").key == "ASPH-SPA_BLACK", "ignoring case");
    Require(Kn5Importer::MatchSurface(surfaces, "3ASPH2").key == "ASPH", "a shorter key when it is the only one");
    match = Kn5Importer::MatchSurface(surfaces, "01WALL003");
    Require(match.key == "WALL", "a name no surface matches is its own, without the digits around it");
    RequireNear(match.friction, kUnknownSurfaceFriction, 1e-6f, "at the unknown surface's friction");

    // Perceptual roughness: alpha = sqrt(2 / (n + 2)) is Blinn-Phong's width, and the shader squares
    // the roughness to get alpha.
    RequireNear(Kn5Importer::SpecularExponentToRoughness(0.0f), 1.0f, 1e-6f, "exponent 0 is fully rough");
    RequireNear(Kn5Importer::SpecularExponentToRoughness(100.0f), 0.37420f, 1e-4f, "exponent 100");
    RequireNear(Kn5Importer::SpecularExponentToRoughness(2000.0f), 0.17778f, 1e-4f, "a paint's sun exponent");
    RequireNear(Kn5Importer::SpecularExponentToRoughness(1e6f), 0.04f, 1e-6f, "roughness floor");

    // The detail's colour as AC multiplies it, in gamma space: Kunos' shader does not double it.
    std::vector<std::uint8_t> grey(4 * 4, 148);
    const auto white = Kn5Importer::FlatDetailColor(grey, 2, 2);
    Require(white.has_value(), "a flat grey is a colour");
    RequireNear((*white)[0], 0.58039f, 1e-4f, "the detail's own value");
    std::vector<std::uint8_t> red{126, 1, 0, 255, 128, 2, 1, 255};
    const auto soulRed = Kn5Importer::FlatDetailColor(red, 2, 1);
    Require(soulRed.has_value(), "within 6 levels is one colour");
    RequireNear((*soulRed)[0], 0.49804f, 1e-4f, "the middle of the range, in gamma space");
    std::vector<std::uint8_t> pattern{0, 0, 0, 255, 60, 0, 0, 255};
    Require(!Kn5Importer::FlatDetailColor(pattern, 2, 1).has_value(), "a pattern is not a paint colour");

    // AC's diffuse level: 2 at 0.5 / 0.5 (Kunos' paint), the sun term weighing about twice the
    // ambient one.
    RequireNear(Kn5Importer::DiffuseGain(0.5f, 0.5f), 2.0f, 1e-5f, "a neutral surface");
    RequireNear(Kn5Importer::DiffuseGain(0.15f, 0.2f), 0.66977f, 1e-4f, "the R34's leather");
    RequireNear(Kn5Importer::DiffuseGain(1.0f, 0.0f), 2.60465f, 1e-4f, "the sun term alone");
    RequireNear(Kn5Importer::DiffuseGain(0.0f, 0.0f), 0.0f, 0.0f, "no diffuse at all");
    // The mean of AC's reflection weight over the hemisphere, cosine-weighted.
    RequireNear(Kn5Importer::MeanReflection(0.0f, 1.0f, 1.0f, 1), 1.0f / 3.0f, 1e-4f, "(1 - N.V) averages 1/3");
    RequireNear(Kn5Importer::MeanReflection(0.0f, 0.5f, 1.0f, 0), 1.0f / 3.0f, 1e-4f,
                "isAdditive 0 raises an exponent below 1 to 1");
    // Capped at 0.2 below N.V 0.8: 2 x (0.2 x 0.32 + the integral of (1 - mu) mu from 0.8 to 1).
    RequireNear(Kn5Importer::MeanReflection(0.0f, 1.0f, 0.2f, 1), 0.16267f, 1e-4f, "fresnelMaxLevel caps it");
    RequireNear(Kn5Importer::MeanReflection(0.05f, 0.2f, 0.7f, 2), 0.67628f, 1e-3f,
                "the R34's headlight chrome sits at its cap");
}

void ConversionReportsProgressToCompletion()
{
    ScopedDirectory scope;
    const std::filesystem::path kn5 = WriteCarFolder(scope.Path());
    std::vector<float> reported;
    Kn5Importer::ConvertToGltf(
        kn5,
        scope.Path() / "assets" / "progress",
        {},
        [&](float fraction)
        {
            reported.push_back(fraction);
        });

    Require(!reported.empty(), "a conversion reported no progress");
    Require(reported.back() == 1.0f, "a conversion did not end at 100%");
    for (size_t index = 0; index < reported.size(); ++index)
    {
        Require(reported[index] >= 0.0f && reported[index] <= 1.0f, "progress left [0, 1]");
        Require(index == 0 || reported[index] >= reported[index - 1], "progress went backwards");
    }
}

// Textures are decoded and written on several threads; the schedule must not show in the result.
void ConversionIsDeterministicAcrossTextureThreads()
{
    ScopedDirectory scope;
    const std::filesystem::path kn5 = WriteCarFolder(scope.Path());
    const Kn5ImportReport first = Kn5Importer::ConvertToGltf(kn5, scope.Path() / "assets" / "first");
    const Kn5ImportReport second = Kn5Importer::ConvertToGltf(kn5, scope.Path() / "assets" / "second");
    Require(first.images > 1, "the fixture needs several textures for this to mean anything");

    const auto readAll = [](const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    Require(readAll(first.gltfPath.parent_path() / "buffers" / "first.bin").size() ==
                readAll(second.gltfPath.parent_path() / "buffers" / "second.bin").size(),
            "the buffers differ in size");
    size_t compared = 0;
    for (const auto& entry : std::filesystem::directory_iterator(scope.Path() / "assets" / "first" / "textures"))
    {
        const std::filesystem::path other = scope.Path() / "assets" / "second" / "textures" / entry.path().filename();
        Require(std::filesystem::exists(other), "texture " + entry.path().filename().string() + " is missing from the second import");
        Require(readAll(entry.path()) == readAll(other), "texture " + entry.path().filename().string() + " differs between imports");
        ++compared;
    }
    Require(compared == first.images, "every image is a file in textures/");
}

void ConvertsHierarchyGeometryAndMaterials()
{
    ScopedDirectory scope;
    const std::filesystem::path kn5 = WriteCarFolder(scope.Path());
    Require(ModelLoader::IsImportableModelPath(kn5) && !ModelLoader::IsSupportedModelPath(kn5),
            "a kn5 is imported, not loaded");
    Require(Kn5Importer::ListSkins(kn5) == std::vector<std::string>{"00_soul_red", "01_arctic_white"}, "skins listed");

    const std::filesystem::path bundle = scope.Path() / "assets" / "fixture_lod_a";
    const Kn5ImportReport report = Kn5Importer::ConvertToGltf(kn5, bundle);
    Require(report.gltfPath == bundle / "fixture_lod_a.gltf", "the glTF is named after the kn5");
    Require(std::filesystem::exists(bundle / "buffers" / "fixture_lod_a.bin"), "the buffer is under buffers/");
    Require(report.skin == "00_soul_red" && report.skinTextures == 1, "the first skin is the default");
    Require(report.droppedVariants == 3, "BODY_DAMAGE, RIM_BLUR_LF and STEER_LR are dropped");
    Require(report.meshes == 5 && report.transforms == 3, "mesh and transform counts");
    Require(report.scrubbedMatrices == 1, "the NaN matrix is dropped");

    const LoadedModelData model = ModelLoader::LoadModel(report.gltfPath.string());
    Require(model.submeshes.size() == 5, "five meshes load, got " + std::to_string(model.submeshes.size()));
    for (const char* kept : {"BODY", "RIM_LF", "STEER_HR", "WHEEL_LR", "GEAR"})
    {
        Require(HasSubmesh(model, kept), std::string(kept) + " is kept");
    }
    for (const char* dropped : {"BODY_DAMAGE", "RIM_BLUR_LF", "STEER_LR"})
    {
        Require(!HasSubmesh(model, dropped), std::string(dropped) + " is dropped");
    }

    // AC is +X left, +Z forward; the root's half turn about Y makes it glTF's +X right, -Z forward.
    const ModelSubmeshData& body = FindSubmesh(model, "BODY");
    RequireNear(body.mesh.vertices[0].position[0], -1.0f, 1e-5f, "body x is mirrored by the half turn");
    RequireNear(body.mesh.vertices[2].position[2], -1.0f, 1e-5f, "body z is mirrored by the half turn");
    RequireNear(body.mesh.vertices[0].normal[1], 1.0f, 1e-5f, "normals are unit length");
    // The dummy's placement is kept: the rim sits at the wheel centre.
    const ModelSubmeshData& rim = FindSubmesh(model, "RIM_LF");
    RequireNear(rim.mesh.vertices[0].position[0], -0.7f, 1e-5f, "wheel x");
    RequireNear(rim.mesh.vertices[0].position[1], 0.3f, 1e-5f, "wheel y");
    RequireNear(rim.mesh.vertices[0].position[2], -1.2f, 1e-5f, "wheel z");
    // UVs are not flipped: kn5 and glTF share a top-left origin.
    RequireNear(body.mesh.vertices[1].texCoord[1], 1.0f, 1e-6f, "uv v is kept");

    // Texture paths are relative to the model file, as for any imported glTF.
    const std::filesystem::path folder = report.gltfPath.parent_path();
    Require(model.materials.size() == 4, "every material converts");
    const ModelMaterialData& paint = model.materials[0];
    Require(paint.name == "EXT_Carpaint", "material name");
    // The livery's paint (126,1,0), not the kn5's grey template.
    RequireNear(paint.baseColor[0], 0.97345f, 1e-4f, "paint red");
    RequireNear(paint.baseColor[1], 0.00061f, 1e-4f, "paint green");
    RequireNear(paint.baseColor[2], 0.0f, 1e-6f, "paint blue");
    RequireNear(paint.metallicFactor, 0.0f, 0.0f, "AC is dielectric");
    RequireNear(paint.roughnessFactor, 1.0f, 0.0f, "a per-pixel roughness takes over from the factor");
    Require(!paint.roughnessTexturePath.empty() && std::filesystem::exists(folder / paint.roughnessTexturePath),
            "txMaps is baked to a roughness map");
    // Car paint (isAdditive 2): the base keeps the ksSpecular lobe, masked by txMaps' R; the
    // reflection is the coat.
    RequireNear(paint.specularFactor, 0.6f, 1e-4f, "ksSpecular is the paint base's specular level");
    Require(paint.specularTexturePath == paint.roughnessTexturePath, "one baked map is roughness and specular mask");
    RequireNear(paint.specularColorFactor[0], 0.2706f, 1e-3f, "a solid livery's detail alpha (69) scales F0");
    Require(paint.specularColorTexturePath.empty(), "no flake map for a flat alpha");
    int width = 0;
    int height = 0;
    const std::vector<std::uint8_t> base = ReadPngRgba((folder / paint.roughnessTexturePath).string(), width, height);
    Require(width == 4 && height == 4, "the base map keeps txMaps' size");
    Require(base[1] == 100, "full gloss at exponent 80 (80 + 1 in AC) is roughness 0.394");
    Require(base[2] == 0, "dielectric");
    Require(base[5 * 4 + 3] == 85, "the specular mask is txMaps' R");
    RequireNear(paint.clearcoatFactor, 1.0f, 0.0f, "the coat's weight is in its map");
    Require(!paint.clearcoatTexturePath.empty() && paint.clearcoatRoughnessTexturePath == paint.clearcoatTexturePath,
            "one baked map is the coat's weight and roughness");
    const std::vector<std::uint8_t> coat = ReadPngRgba((folder / paint.clearcoatTexturePath).string(), width, height);
    Require(coat[0] == 238, "fresnelC 0.07 x reflection 136/255 over the coat's 0.04 is weight 0.93");
    Require(coat[1] == 45, "the sun exponent 2000 at full gloss is coat roughness 0.178");
    Require(paint.normalTexturePath.empty(), "a damage dent map is not bound as a normal map");
    Require(paint.alphaMode == MaterialAlphaMode::Opaque, "paint is opaque");
    Require(std::filesystem::exists(folder / paint.baseColorTexturePath), "diffuse written");
    Require(PngChannels(folder / paint.baseColorTexturePath) == 3, "an opaque material's diffuse loses its alpha");

    const ModelMaterialData& leaves = model.materials[1];
    Require(leaves.alphaMode == MaterialAlphaMode::Mask, "alpha-tested is MASK");
    RequireNear(leaves.alphaCutoff, 0.5f, 0.0f, "ksAlphaRef 0 falls back to 0.5");
    RequireNear(leaves.roughnessFactor, 0.54912f, 1e-4f, "the default exponent 20");
    RequireNear(leaves.specularFactor, 0.0f, 0.0f, "ksSpecular 0 reflects nothing");
    RequireNear(leaves.clearcoatFactor, 0.0f, 0.0f, "only car paint has a coat");
    Require(PngChannels(folder / leaves.baseColorTexturePath) == 4, "an alpha-tested diffuse keeps its alpha");

    const ModelMaterialData& glass = model.materials[2];
    Require(glass.alphaMode == MaterialAlphaMode::Blend, "alpha-blended is BLEND");
    Require(!glass.baseColorTexturePath.empty(), "a texture named in a different case still resolves");
    RequireNear(glass.roughnessFactor, 0.31544f, 1e-4f, "exponent 200, whatever the intensity");
    RequireNear(glass.specularFactor, 0.5f, 1e-6f, "the intensity is the specular level");
    Require(PngChannels(folder / glass.baseColorTexturePath) == 3, "a uniformly opaque alpha is dropped");

    const ModelMaterialData& lamp = model.materials[3];
    Require(lamp.baseColorTexturePath.empty(), "a stub texture is not written");
    RequireNear(lamp.emissiveColor[0], 1.0f, 0.0f, "a grey ksEmissive is white");
    RequireNear(lamp.emissiveIntensity, 40955.0f, 60.0f, "ksEmissive 3 is (3 x 2 / 4.84)^2.2 x 25 500 cd/m^2");
}

void SkinChoiceChangesThePaint()
{
    ScopedDirectory scope;
    const std::filesystem::path kn5 = WriteCarFolder(scope.Path());

    Kn5ImportOptions embedded;
    embedded.skin = "none";
    const Kn5ImportReport template_ = Kn5Importer::ConvertToGltf(kn5, scope.Path() / "none", embedded);
    Require(template_.skin.empty(), "'none' keeps the kn5's textures");
    const LoadedModelData grey = ModelLoader::LoadModel(template_.gltfPath.string());
    // The kn5's grey detail (148) doubles to 1.16: above 1, so the product is baked, 174 x 1.16.
    RequireNear(grey.materials[0].baseColor[0], 1.0f, 1e-6f, "a baked paint has no factor");
    int width = 0;
    int height = 0;
    const std::filesystem::path greyFolder = template_.gltfPath.parent_path();
    const std::vector<std::uint8_t> greyPaint =
        ReadPngRgba((greyFolder / grey.materials[0].baseColorTexturePath).string(), width, height);
    Require(greyPaint[0] == 202, "the template times the doubled grey, unclamped until the product");

    Kn5ImportOptions white;
    white.skin = "01_ARCTIC_WHITE";
    const Kn5ImportReport chosen = Kn5Importer::ConvertToGltf(kn5, scope.Path() / "white", white);
    Require(chosen.skin == "01_arctic_white", "skins match ignoring case");
    const LoadedModelData arctic = ModelLoader::LoadModel(chosen.gltfPath.string());
    const ModelMaterialData& whitePaint = arctic.materials[0];
    Require(!whitePaint.specularColorTexturePath.empty(), "a metallic livery's alpha noise is the flake map");
    RequireNear(whitePaint.textureTransforms[static_cast<size_t>(MaterialTextureSlot::SpecularColor)].scale[0], 40.0f, 1e-6f,
                "the flakes tile like the detail");
    const std::vector<std::uint8_t> flakes =
        ReadPngRgba((chosen.gltfPath.parent_path() / whitePaint.specularColorTexturePath).string(), width, height);
    Require(flakes[0] == 96 && flakes[4] == 175, "alpha 30 and 110, sRGB encoded for the colour slot");

    Kn5ImportOptions missing;
    missing.skin = "02_nope";
    RequireThrows([&]
                  {
                      Kn5Importer::ConvertToGltf(kn5, scope.Path() / "missing", missing);
                  },
                  "an unknown skin");

    Kn5ImportOptions keep;
    keep.keepVariants = true;
    const Kn5ImportReport all = Kn5Importer::ConvertToGltf(kn5, scope.Path() / "all", keep);
    Require(all.droppedVariants == 0 && all.meshes == 8, "keepVariants keeps every mesh");
}

void RefusesEncryptedAndExistingTargets()
{
    ScopedDirectory scope;
    const std::filesystem::path kn5 = WriteCarFolder(scope.Path());

    std::vector<std::uint8_t> encrypted = BuildCarKn5(6);
    const std::string marker = Kn5Reader::kEncryptionMarker;
    encrypted.insert(encrypted.end(), marker.begin(), marker.end());
    const std::filesystem::path encryptedPath = scope.Path() / "enc" / "car.kn5";
    WriteFile(encryptedPath, encrypted);
    Require(Kn5Reader::IsEncrypted(encryptedPath), "the CSP trailer is detected");
    // Across the scanner's 1 MiB chunks.
    std::vector<std::uint8_t> straddling((1u << 20) - 10, 0);
    straddling.insert(straddling.end(), marker.begin(), marker.end());
    WriteFile(scope.Path() / "enc" / "straddling.kn5", straddling);
    Require(Kn5Reader::IsEncrypted(scope.Path() / "enc" / "straddling.kn5"), "a trailer across a chunk boundary is found");
    Require(!Kn5Reader::IsEncrypted(scope.Path() / "ks_fixture" / "fixture_lod_a.kn5"), "a plain file is not encrypted");
    RequireThrows([&]
                  {
                      Kn5Importer::ConvertToGltf(encryptedPath, scope.Path() / "enc_out");
                  },
                  "an encrypted kn5");

    const std::filesystem::path bundle = scope.Path() / "twice";
    Kn5Importer::ConvertToGltf(kn5, bundle);
    RequireThrows([&]
                  {
                      Kn5Importer::ConvertToGltf(kn5, bundle);
                  },
                  "converting over an existing import");
}

void ImportGoesThroughTheModelLoader()
{
    ScopedDirectory scope;
    const std::filesystem::path kn5 = WriteCarFolder(scope.Path());
    const std::filesystem::path bundle = scope.Path() / "models" / "fixture_lod_a";
    std::filesystem::create_directories(bundle);
    const std::filesystem::path imported = ModelLoader::CopyModelWithSortedReferences(kn5, bundle);
    Require(imported == bundle / "fixture_lod_a.gltf", "the import returns the converted glTF");
    Require(ModelLoader::IsSupportedModelPath(imported), "the imported model is loadable");
    Require(ModelLoader::LoadModel(imported.string()).submeshes.size() == 5, "the imported model loads");
    RequireThrows([&]
                  {
                      ModelLoader::LoadModel(kn5.string());
                  },
                  "loading a kn5 without importing it");

    // The import dialog's choices reach the converter.
    Kn5ImportOptions embedded;
    embedded.skin = "none";
    const std::filesystem::path other = scope.Path() / "models" / "embedded";
    std::filesystem::create_directories(other);
    const std::filesystem::path withOptions = ModelLoader::CopyModelWithSortedReferences(kn5, other, embedded);
    RequireNear(ModelLoader::LoadModel(withOptions.string()).materials[0].baseColor[0], 1.0f, 1e-6f,
                "the chosen skin is the one converted");
}

void InspectOffersLiveriesAndOptions()
{
    ScopedDirectory scope;
    const std::filesystem::path kn5 = WriteCarFolder(scope.Path());
    const Kn5ModelSummary summary = Kn5Importer::Inspect(kn5);
    Require(!summary.encrypted, "a plain kn5 is not encrypted");
    Require(summary.meshes == 8 && summary.triangles == 8, "every mesh in the file is counted");
    Require(summary.materials == 4 && summary.textures == 7, "material and texture counts");
    Require(summary.runtimeVariants == 3, "the dropped subtrees are counted");
    Require(summary.skins.size() == 3, "two skins and the embedded textures");
    Require(summary.skins[0].name == "00_soul_red" && summary.skins[1].name == "01_arctic_white" &&
                summary.skins[2].name.empty(),
            "skins in the game's order, the embedded textures last");

    const Kn5SkinSummary& red = summary.skins[0];
    Require(red.paint.has_value() && *red.paint == std::array<std::uint8_t, 3>{126, 1, 0}, "the livery's paint colour");
    Require(red.paintFromSkin && red.paintMaterial == "EXT_Carpaint", "the paint comes from the livery's texture");
    const Kn5SkinSummary& embedded = summary.skins[2];
    Require(embedded.paint.has_value() && *embedded.paint == std::array<std::uint8_t, 3>{148, 148, 148},
            "the embedded paint is the kn5's template");
    Require(!embedded.paintFromSkin, "the embedded paint comes from the kn5");

    std::vector<std::uint8_t> encrypted = BuildCarKn5(6);
    const std::string marker = Kn5Reader::kEncryptionMarker;
    encrypted.insert(encrypted.end(), marker.begin(), marker.end());
    const std::filesystem::path encryptedPath = scope.Path() / "enc" / "car.kn5";
    WriteFile(encryptedPath, encrypted);
    const Kn5ModelSummary refused = Kn5Importer::Inspect(encryptedPath);
    Require(refused.encrypted && refused.skins.empty(), "an encrypted kn5 is reported, with no decoy colours");
}

void PaintRankingPrefersTheBodywork()
{
    // The rims carry the most triangles, which is why size alone picked them on the MX-5.
    const std::vector<size_t> order = Kn5Importer::RankPaintedMaterials(
        {"RIM", "EXT_Carpaint", "INT_OCC_Carpaint", "Decal", "Glass"},
        {true, true, true, true, false},
        {33024, 23554, 5000, 100, 0});
    Require(order == std::vector<size_t>{1, 2, 3, 0}, "body first, interior copy next, unknown, then the rim");
}
}

// A one-mesh kn5 whose material reads `diffuse`, and which carries the textures in `textures`.
std::vector<std::uint8_t> BuildTrackKn5(
    const std::string& meshName,
    const std::string& materialName,
    const std::string& diffuse,
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& textures,
    bool withCollisionMesh = false)
{
    ByteWriter writer;
    writer.Raw("sc6969", 6);
    writer.U32(5);
    writer.U32(static_cast<std::uint32_t>(textures.size()));
    for (const auto& [name, data] : textures)
    {
        writer.U32(1);
        writer.String(name);
        writer.Blob(data);
    }
    writer.U32(1);
    WriteMaterial(writer, {materialName, "ksPerPixel", false, false, {{"ksSpecular", 0.0f}}, {{"txDiffuse", diffuse}}});
    WriteDummy(writer, meshName + "_ROOT", withCollisionMesh ? 4 : 1, kIdentity);
    WriteMesh(writer, meshName, 0, 1.0f);
    if (withCollisionMesh)
    {
        // A physics-only surface, as tracks ship them: the game collides with it but never draws it.
        WriteMesh(writer, meshName + "_PHYSICS", 0, 7.0f, false, 0, false);
        // A pit box marker: a dummy with a unit cube of the same name under it, never drawn.
        WriteDummy(writer, "AC_PIT_0", 1, kIdentity);
        WriteMesh(writer, "AC_PIT_0", 0, 8.0f);
        // The far LOD of the road, drawn only from 300 m out, where the near one stops.
        WriteMesh(writer, meshName + "_FAR", 0, 9.0f, false, 0, true, 300.0f);
    }
    return writer.Bytes();
}

// A track folder: the main kn5, a second one in a subfolder, and a layout placing both. The trees'
// material reads a texture only the main kn5 carries, as track add-on files do.
std::filesystem::path WriteTrackFolder(const std::filesystem::path& root)
{
    const std::filesystem::path track = root / "ks_fixture_track";
    WriteFile(track / "ks_fixture_track.kn5",
              BuildTrackKn5("1ROAD", "asphalt", "asphalt.dds", {{"asphalt.dds", DdsFlat(4, {60, 60, 60, 255})}, {"leaf.dds", DdsFlat(4, {20, 90, 20, 255})}}, true));
    WriteFile(track / "extra" / "trees.kn5", BuildTrackKn5("TREE", "trees", "leaf.dds", {}));
    const std::string layout =
        "; the east layout\n"
        "[MODEL_0]\n"
        "FILE=ks_fixture_track.kn5\n"
        "\n"
        "[MODEL_1]\n"
        "FILE=extra/trees.kn5\n"
        "POSITION=10, 0, 5\n"
        "ROTATION=0,90,0\n"
        "[SOMETHING_ELSE]\n"
        "FILE=ignored.kn5\n";
    WriteFile(track / "models_east.ini", std::vector<std::uint8_t>(layout.begin(), layout.end()));
    const std::string surfaces = "[SURFACE_0]\nKEY=ROAD\nFRICTION=0.77\nDAMPING=0\n";
    WriteFile(track / "data" / "surfaces.ini", std::vector<std::uint8_t>(surfaces.begin(), surfaces.end()));
    return track;
}

void ReadsTrackLayouts()
{
    ScopedDirectory scope;
    const std::filesystem::path track = WriteTrackFolder(scope.Path());
    const std::filesystem::path layoutPath = track / "models_east.ini";

    Require(Kn5Importer::IsLayoutPath(layoutPath) && Kn5Importer::IsLayoutPath("C:/t/MODELS.INI"), "models*.ini is a layout");
    Require(!Kn5Importer::IsLayoutPath(track / "surfaces.ini"), "another ini is not a layout");
    Require(ModelLoader::IsImportableModelPath(layoutPath), "a layout is importable");
    Require(ModelLoader::ImportName(layoutPath) == "ks_fixture_track_east", "a layout is named after its track");
    Require(ModelLoader::ImportName(track / "models.ini") == "ks_fixture_track", "the default layout takes the track's name");

    const std::vector<Kn5LayoutModel> models = Kn5Importer::ReadLayout(layoutPath);
    Require(models.size() == 2, "every [MODEL_n] with a FILE, and nothing else");
    Require(models[1].file.filename() == "trees.kn5" && models[1].position == std::array<float, 3>{10.0f, 0.0f, 5.0f},
            "FILE is relative to the ini; POSITION is read");
    Require(models[1].rotationDegrees == std::array<float, 3>{0.0f, 90.0f, 0.0f} && models[0].position == std::array<float, 3>{},
            "ROTATION is read; a missing POSITION is zero");

    const std::vector<std::filesystem::path> layouts = Kn5Importer::FindLayouts(track / "ks_fixture_track.kn5");
    Require(layouts.size() == 1 && layouts[0].filename() == "models_east.ini", "the layouts placing a kn5 are found");
    Require(Kn5Importer::FindLayouts(track / "extra" / "trees.kn5").empty(), "only layouts in the kn5's own folder are offered");

    const std::string broken = "[MODEL_0]\nFILE=missing.kn5\nPOSITION=a,b,c\n";
    WriteFile(track / "models_broken.ini", std::vector<std::uint8_t>(broken.begin(), broken.end()));
    RequireThrows([&]
                  {
                      Kn5Importer::ReadLayout(track / "models_broken.ini");
                  },
                  "a layout naming a missing kn5");
    WriteFile(track / "models_empty.ini", {});
    RequireThrows([&]
                  {
                      Kn5Importer::ReadLayout(track / "models_empty.ini");
                  },
                  "a layout placing nothing");
    Require(Kn5Importer::FindLayouts(track / "ks_fixture_track.kn5").size() == 1, "broken layouts are not offered");

    // Rows of Rz * Ry * Rx, stored column-major; 90 degrees about Y takes +X to -Z.
    const std::array<float, 16> matrix = Kn5Importer::LayoutModelMatrix({10.0f, 0.0f, 5.0f}, {0.0f, 90.0f, 0.0f});
    RequireNear(matrix[2], -1.0f, 1e-6f, "the rotated X axis points down -Z");
    RequireNear(matrix[12], 10.0f, 0.0f, "the translation is the last column");
}

void ImportsAWholeTrackLayout()
{
    ScopedDirectory scope;
    const std::filesystem::path track = WriteTrackFolder(scope.Path());
    const std::filesystem::path layoutPath = track / "models_east.ini";

    const Kn5ModelSummary summary = Kn5Importer::Inspect(layoutPath);
    Require(summary.models == 2 && summary.meshes == 3 && summary.materials == 2, "a layout is surveyed across its models");
    Require(summary.hiddenMeshes == 2, "the collision-only mesh and the marker cube are counted apart from the drawn ones");
    Require(summary.runtimeVariants == 1, "the far LOD is a dropped variant");
    Require(summary.skins.size() == 1 && summary.skins[0].name.empty(), "a track offers only its own textures");
    const Kn5ModelSummary main = Kn5Importer::Inspect(track / "ks_fixture_track.kn5");
    Require(main.models == 1 && main.layouts.size() == 1 && main.layouts[0].models == 2, "a kn5 offers the layouts placing it");

    const std::filesystem::path bundle = scope.Path() / "models" / "ks_fixture_track_east";
    std::filesystem::create_directories(bundle);
    const std::filesystem::path imported = ModelLoader::CopyModelWithSortedReferences(layoutPath, bundle);
    Require(imported == bundle / "ks_fixture_track_east.gltf", "a layout imports as one glTF named after it");

    const LoadedModelData model = ModelLoader::LoadModel(imported.string());
    Require(model.submeshes.size() == 2, "both models of the layout load, without the collision-only mesh");
    Require(!HasSubmesh(model, "1ROAD_PHYSICS"), "a mesh the game never renders is not imported");
    Require(!HasSubmesh(model, "AC_PIT_0"), "a track marker's cube is not imported");
    Require(!HasSubmesh(model, "1ROAD_FAR"), "a far LOD is not imported beside the near one");
    const ModelSubmeshData& road = FindSubmesh(model, "1ROAD");
    const ModelSubmeshData& tree = FindSubmesh(model, "TREE");
    // Each mesh keeps the material of its own kn5, though both files index theirs from 0.
    Require(model.materials[road.materialIndex].name == "asphalt", "the main model's material");
    Require(model.materials[tree.materialIndex].name == "trees", "the second model's material, offset past the first's");
    // The trees' texture lives in the main kn5 only.
    Require(!model.materials[tree.materialIndex].baseColorTexturePath.empty(), "a texture another model of the layout carries resolves");

    // The physics mesh is collision only: not a submesh, on the surface its name and the track's
    // surfaces.ini say, in the model's space (the axis root turns its (7, 0, 0) to (-7, 0, 0)).
    Require(model.collisionMeshes.size() == 1, "the physics mesh becomes the model's collision");
    const ModelCollisionMesh& collision = model.collisionMeshes[0];
    Require(collision.surface == "ROAD", "surface " + collision.surface);
    RequireNear(collision.friction, 0.77f, 1e-6f, "friction from the track's surfaces.ini");
    Require(collision.positions.size() == 3 && collision.indices.size() == 3, "one triangle");
    RequireNear(collision.positions[0].x, -7.0f, 1e-5f, "in the model's space");
    Require(model.maxBounds.x < 6.0f, "and outside its bounds");

    // Unplaced: (1, 0, 0) under the axis root only. Placed: rotated to (0, 0, -1), moved by
    // (10, 0, 5) to (10, 0, 4), then the axis root's half turn.
    RequireNear(road.mesh.vertices[0].position[0], -1.0f, 1e-5f, "the unplaced model sits at the origin");
    RequireNear(tree.mesh.vertices[0].position[0], -10.0f, 1e-4f, "placed x");
    RequireNear(tree.mesh.vertices[0].position[2], -4.0f, 1e-4f, "placed z");
}

// Three track surfaces: tarmac on world-space detail maps with a detail normal, a kerb on UV-space
// ones, and a plain wall.
std::vector<std::uint8_t> BuildSurfacesKn5()
{
    ByteWriter writer;
    writer.Raw("sc6969", 6);
    writer.U32(5);
    const std::vector<std::string> textures{"asph.dds", "asph_mask.dds", "tarmac_detail.dds", "grass_detail.dds",
                                            "tarmac_nm.dds", "kerb.dds", "wall.dds"};
    writer.U32(static_cast<std::uint32_t>(textures.size()));
    for (const std::string& name : textures)
    {
        writer.U32(1);
        writer.String(name);
        writer.Blob(DdsFlat(4, {120, 120, 120, 255}));
    }
    const std::vector<FixtureMaterial> materials{
        {"asph", "ksMultilayer_fresnel_nm", false, false,
         {{"ksSpecular", 0.0f}, {"multR", 0.8f}, {"multG", 0.25f}, {"multB", 0.0f}, {"multA", 0.0f}, {"magicMult", 1.2f}, {"detailNMMult", 5.0f}},
         {{"txDiffuse", "asph.dds"}, {"txMask", "asph_mask.dds"}, {"txDetailR", "tarmac_detail.dds"}, {"txDetailG", "grass_detail.dds"},
          {"txDetailB", "tarmac_detail.dds"}, {"txDetailA", "tarmac_detail.dds"}, {"txDetailNM", "tarmac_nm.dds"}}},
        {"kerb", "ksMultilayer_objsp", false, false,
         {{"multR", 12.0f}, {"multG", 3.0f}, {"multB", 1.0f}, {"multA", 1.0f}},
         {{"txDiffuse", "kerb.dds"}, {"txMask", "asph_mask.dds"}, {"txDetailR", "tarmac_detail.dds"}, {"txDetailG", "grass_detail.dds"},
          {"txDetailB", "tarmac_detail.dds"}, {"txDetailA", "tarmac_detail.dds"}}},
        {"wall", "ksPerPixel", false, false, {{"ksSpecular", 0.2f}}, {{"txDiffuse", "wall.dds"}}},
        // Spa's tarmac: a broad exponent, intensity above 1 and a sheen multiplier that once
        // sharpened the lobe to roughness 0.225.
        {"glossy_tarmac", "ksMultilayer_fresnel_nm", false, false,
         {{"ksSpecular", 1.2f}, {"ksSpecularEXP", 15.0f}, {"fresnelMaxLevel", 2.5f}, {"tarmacSpecularMultiplier", 2.5f}},
         {{"txDiffuse", "asph.dds"}, {"txMask", "asph_mask.dds"}, {"txDetailR", "tarmac_detail.dds"}}},
    };
    writer.U32(static_cast<std::uint32_t>(materials.size()));
    for (const FixtureMaterial& material : materials)
    {
        WriteMaterial(writer, material);
    }
    WriteDummy(writer, "ROOT", 4, kIdentity);
    WriteMesh(writer, "ROAD", 0, 1.0f);
    WriteMesh(writer, "KERB", 1, 2.0f);
    WriteMesh(writer, "WALL", 2, 3.0f);
    WriteMesh(writer, "PIT", 3, 4.0f);
    return writer.Bytes();
}

// ---- A car's own data ---------------------------------------------------------------------------------

// The data files of a 718 Boxster S PDK, trimmed to what the import reads (values as the game ships them).
std::map<std::string, std::string> BoxsterDataFiles()
{
    return {
        {"car.ini",
         "[HEADER]\r\nVERSION=2 ; version number\r\n\r\n[BASIC]\r\nGRAPHICS_OFFSET=0,-0.33,0.11 ; correction\r\nTOTALMASS=1460 ; kg with driver\r\n\r\n"
         "[FUEL]\r\nFUEL=30 ; default starting fuel in litres\r\nMAX_FUEL=64\r\n\r\n[FUELTANK]\r\nPOSITION=0,-0.15,-0.85\r\n\r\n"
         "[CONTROLS]\r\nSTEER_LOCK=400 ; real car's lock from centre to right\r\nSTEER_RATIO=15.0\r\n"
         "[GRAPHICS]\r\nDRIVEREYES=0.35,1.05,-0.30\r\nON_BOARD_PITCH_ANGLE=-4\r\n"},
        {"engine.ini",
         "[HEADER]\r\nVERSION=1\r\nPOWER_CURVE=power.lut ; power curve file\r\n\r\n[ENGINE_DATA]\r\nINERTIA=0.137 ; kg m^2\r\nLIMITER=7500 ; rev limiter\r\nMINIMUM=900\r\n\r\n"
         "[COAST_REF]\r\nRPM=7500\r\nTORQUE=90\r\n\r\n"
         "[TURBO_0]\r\nLAG_DN=0.996\r\nLAG_UP=0.99\r\nMAX_BOOST=1.2\r\nWASTEGATE=1.1\r\nREFERENCE_RPM=1900\r\nGAMMA=2\r\n"},
        {"power.lut", "0|50\r\n500|110\r\n1000|135\r\n1500|176\r\n1900|184\r\n2000|184\r\n4500|183\r\n6500|165\r\n7500|127\r\n8500|0\r\n"},
        // A hybrid's ERS (the McLaren P1's ers.ini, shortened), borrowed so the import carries one.
        {"ers.ini",
         "[HEADER]\r\nVERSION=1\r\n[KINETIC]\r\nCHARGE_K=0.00014 ; charge\r\nTORQUE_CURVE=kers_torque.lut ; Nm/RPM\r\nCOAST_CURVE=kers_torque_coast.lut\r\n"
         "DISCHARGE_TIME=87000 ; ms\r\nHAS_BUTTON_OVERRIDE=1\r\nMAX_KJ_PER_LAP=10000000\r\nDEFAULT_CONTROLLER=0\r\nBRAKE_REAR_CORRECTION=1\r\n"
         "[HEAT]\r\nCHARGE_K=0.002\r\nTORQUE_PERC=10\r\n"},
        {"kers_torque.lut", "0|260\r\n4000|260\r\n6000|214\r\n8000|178\r\n8300|0\r\n"},
        {"kers_torque_coast.lut", "0|0\r\n9000|0\r\n"},
        {"ctrl_ers_0.ini",
         "[HEADER]\r\nNAME=Race\r\n[CONTROLLER_0]\r\nCOMBINATOR=ADD ; mode\r\nINPUT=GAS\r\nLUT=kers_gas.lut\r\nFILTER=0.96\r\nUP_LIMIT=1\r\nDOWN_LIMIT=-1\r\n"
         "[CONTROLLER_1]\r\nCOMBINATOR=MULT\r\nINPUT=GEAR\r\nLUT=kers_gear.lut\r\nFILTER=0.96\r\nUP_LIMIT=1\r\nDOWN_LIMIT=-1\r\n"},
        {"ctrl_ers_1.ini", "[HEADER]\r\nNAME=Charging\r\n[CONTROLLER_0]\r\nCOMBINATOR=ADD\r\nINPUT=GAS\r\nLUT=kers_gas.lut\r\nUP_LIMIT=1\r\nDOWN_LIMIT=0\r\n"},
        {"kers_gas.lut", "0|0\r\n1|1\r\n"},
        {"kers_gear.lut", "0|0\r\n1|0.5\r\n2|1\r\n"},
        {"drivetrain.ini",
         "[TRACTION]\r\nTYPE=RWD ; wheel drive\r\n\r\n[GEARS]\r\nCOUNT=7\r\nGEAR_R=-3.55\r\nGEAR_1=3.91\r\nGEAR_2=2.29\r\nGEAR_3=1.65\r\nGEAR_4=1.30\r\n"
         "GEAR_5=1.08\r\nGEAR_6=0.88\r\nGEAR_7=0.62\r\nFINAL=3.62\r\n\r\n[DIFFERENTIAL]\r\nPOWER=0.25\r\nCOAST=0.40\r\nPRELOAD=5\r\n\r\n"
         "[GEARBOX]\r\nCHANGE_UP_TIME=30\r\nCHANGE_DN_TIME=160\r\nAUTO_CUTOFF_TIME=35\r\n\r\n[CLUTCH]\r\nMAX_TORQUE=700\r\n\r\n"
         "[AUTOCLUTCH]\r\nUPSHIFT_PROFILE=NONE\r\nDOWNSHIFT_PROFILE=DOWNSHIFT_PROFILE\r\nMIN_RPM=1200\r\nMAX_RPM=1800\r\n\r\n"
         "[DOWNSHIFT_PROFILE]\r\nPOINT_0=50\r\nPOINT_1=170\r\n"},
        {"brakes.ini", "[HEADER]\r\nVERSION=1\r\n[DATA]\r\nMAX_TORQUE=3200\r\nFRONT_SHARE=0.65\r\nHANDBRAKE_TORQUE=2000\r\n"},
        {"suspensions.ini",
         "[BASIC]\r\nWHEELBASE=2.475\r\nCG_LOCATION=0.455\r\n[ARB]\r\nFRONT=30000\r\nREAR=16000\r\n[FRONT]\r\nTYPE=STRUT\r\nHUB_MASS=70\r\nSPRING_RATE=30760\r\n"
         "DAMP_BUMP=3273\r\nDAMP_REBOUND=5875\r\nBASEY=-0.105\r\nTRACK=1.515\r\nSTATIC_CAMBER=-1.6\r\nTOE_OUT=-0.00030\r\nBUMP_STOP_RATE=72000\r\nBUMPSTOP_UP=0.080\r\nROD_LENGTH=0.045\r\nPACKER_RANGE=0.090\r\n"
         "BUMPSTOP_DN=0.080\r\nDAMP_FAST_BUMP=1934\r\nDAMP_FAST_BUMPTHRESHOLD=0.080\r\nDAMP_FAST_REBOUND=2601\r\nDAMP_FAST_REBOUNDTHRESHOLD=0.130\r\n"
         "STRUT_CAR=0.28497, 0.40218, -0.08294\r\nSTRUT_TYRE=0.10784, -0.16402, 0.01798\r\nWBCAR_BOTTOM_FRONT=0.43800, -0.16775, 0.26073\r\n"
         "WBCAR_BOTTOM_REAR=0.41057, -0.15672, -0.01280\r\nWBTYRE_BOTTOM=0.10784, -0.16402, 0.01798\r\nWBCAR_STEER=0.48843, -0.09289, 0.10865\r\n"
         "WBTYRE_STEER=0.09707, -0.08479, 0.14781\r\n"
         "[REAR]\r\nTYPE=STRUT\r\nHUB_MASS=80\r\nSPRING_RATE=42500\r\nDAMP_BUMP=4273\r\nDAMP_REBOUND=6873\r\nTRACK=1.540\r\n"
         "STRUT_CAR=0.3355, 0.4601, -0.0506\r\nSTRUT_TYRE=0.1106, -0.1804, 0.0095\r\nWBCAR_BOTTOM_FRONT=0.2688, -0.0641, 0.6175\r\n"
         "WBCAR_BOTTOM_REAR=0.4054, -0.1744, -0.0565\r\nWBTYRE_BOTTOM=0.1106, -0.1804, 0.0095\r\nWBCAR_STEER=0.5410, -0.1257, 0.1950\r\n"
         "WBTYRE_STEER=0.2082, -0.1351, 0.2028\r\n"},
        {"aero.ini",
         "[HEADER]\r\nVERSION=2\r\n[WING_0]\r\nNAME=BODY\r\nCHORD=1\r\nSPAN=1.99\r\nPOSITION=0,0.15,-0.10\r\nLUT_AOA_CL=wing_body_AOA_CL.lut\r\nLUT_GH_CL=\r\n"
         "CL_GAIN=1\r\nLUT_AOA_CD=wing_body_AOA_CD.lut\r\nCD_GAIN=1\r\nANGLE=0\r\nZONE_FRONT_CD=0.005\r\n"
         "[WING_1]\r\nNAME=REAR\r\nCHORD=1\r\nSPAN=1.99\r\nPOSITION=0,0.34 ,-1.600\r\nLUT_AOA_CL=wing_rear_AOA_CL.lut\r\nCL_GAIN=1.0\r\n"
         "LUT_AOA_CD=wing_rear_AOA_CD.lut\r\nCD_GAIN=1.0\r\nANGLE=2\r\n"
         "[DYNAMIC_CONTROLLER_0]\r\nWING=1\r\nCOMBINATOR=ADD\r\nINPUT=SPEED_KMH\r\nLUT=r_wing_controller_speed.lut\r\nFILTER=0.998\r\nUP_LIMIT=10\r\nDOWN_LIMIT=0\r\n"},
        {"wing_body_aoa_cl.lut", "-2|-0.00\r\n0|-0.00\r\n2|0.03\r\n5|0.04\r\n"},
        {"wing_body_aoa_cd.lut", "-2|0.405\r\n0|0.39\r\n2|0.40\r\n5|0.42\r\n"},
        {"wing_rear_aoa_cl.lut", "-2|-0.250\r\n0|-0.230\r\n2|-0.210\r\n4|-0.08\r\n"},
        {"wing_rear_aoa_cd.lut", "-2|0.02\r\n0|0.005\r\n2|0.005\r\n4|0.0\r\n"},
        {"r_wing_controller_speed.lut", "0|0\r\n119|0\r\n120|10\r\n159|10\r\n"},
        {"electronics.ini",
         "[ABS]\r\nSLIP_RATIO_LIMIT=0.11\r\nCURVE=\r\nPRESENT=1\r\nACTIVE=1\r\nRATE_HZ=250\r\n[TRACTION_CONTROL]\r\nSLIP_RATIO_LIMIT=0.08\r\n"
         "CURVE=traction_control.lut\r\nPRESENT=1\r\nACTIVE=1\r\nRATE_HZ=200\r\nMIN_SPEED_KMH=30\r\n[EDL]\r\nPRESENT=1\r\nMAX_SPIN_POWER=0.8\r\n"},
        {"tyres.ini",
         "[HEADER]\r\nVERSION=10\r\n[COMPOUND_DEFAULT]\r\nINDEX=0\r\n"
         "[FRONT]\r\nNAME=Semislicks\r\nSHORT_NAME=SM\r\nWIDTH=0.235\r\nRADIUS=0.336\r\nANGULAR_INERTIA=1.62\r\nDY0=1.3080\r\nDY1=-0.048\r\nDX0=1.3114\r\nDX1=-0.046\r\n"
         "WEAR_CURVE=semislicks_front.lut\r\nFRICTION_LIMIT_ANGLE=7.52\r\nFZ0=3606\r\nLS_EXPY=0.8273\r\nLS_EXPX=0.8915\r\nDY_REF=1.28\r\nDX_REF=1.30\r\nFALLOFF_LEVEL=0.86\r\n"
         "[REAR]\r\nNAME=Semislicks\r\nSHORT_NAME=SM\r\nWIDTH=0.265\r\nRADIUS=0.347\r\nANGULAR_INERTIA=1.97\r\nDY0=1.3114\r\nDY1=-0.048\r\nDX0=1.3195\r\nDX1=-0.046\r\n"
         "WEAR_CURVE=semislicks_rear.lut\r\nFRICTION_LIMIT_ANGLE=7.27\r\nFZ0=3724\r\nLS_EXPY=0.8461\r\nLS_EXPX=0.9065\r\nDY_REF=1.28\r\nDX_REF=1.30\r\nFALLOFF_LEVEL=0.86\r\n"
         "[THERMAL_FRONT]\r\nFRICTION_K=0.04767\r\nPERFORMANCE_CURVE=tcurve_semis.lut\r\n"
         "[THERMAL_REAR]\r\nFRICTION_K=0.04366\r\nPERFORMANCE_CURVE=tcurve_semis.lut\r\n"
         "[FRONT_1]\r\nNAME=Street\r\nSHORT_NAME=ST\r\nANGULAR_INERTIA=1.62\r\nDY0=1.2798\r\nDX0=1.2368\r\nWEAR_CURVE=street_front.lut\r\n"
         "[REAR_1]\r\nNAME=Street\r\nSHORT_NAME=ST\r\nANGULAR_INERTIA=1.97\r\nDY0=1.28\r\nDX0=1.24\r\nWEAR_CURVE=street_rear.lut\r\n"},
        {"semislicks_front.lut", "0|1\r\n5000|0.9\r\n10000|0.7\r\n"},
        {"semislicks_rear.lut", "0|1\r\n5000|0.9\r\n"},
        {"street_front.lut", "0|1\r\n"},
        {"street_rear.lut", "0|1\r\n"},
        {"tcurve_semis.lut", "0|0.5\r\n80|1\r\n150|0.8\r\n"}};
}

// data.acd as the game writes it: a marker and a version word, then per file its name, its size, and one
// 32-bit word per byte, the byte plus the key's.
std::vector<std::uint8_t> BuildAcd(
    const std::string& folderName, int seventh, const std::map<std::string, std::string>& files, bool newFormat = true)
{
    const std::string key = AcdArchive::MakeKey(folderName, seventh);
    std::vector<std::uint8_t> bytes;
    const auto put32 = [&](std::int32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            bytes.push_back(static_cast<std::uint8_t>((static_cast<std::uint32_t>(value) >> shift) & 0xff));
        }
    };
    if (newFormat)
    {
        put32(-1111);
        put32(835647);
    }
    for (const auto& [name, text] : files)
    {
        put32(static_cast<std::int32_t>(name.size()));
        bytes.insert(bytes.end(), name.begin(), name.end());
        put32(static_cast<std::int32_t>(text.size()));
        for (size_t index = 0; index < text.size(); ++index)
        {
            put32(static_cast<std::int32_t>(static_cast<std::uint8_t>(text[index]) + static_cast<std::uint8_t>(key[index % key.size()])));
        }
    }
    return bytes;
}

void AcdKeysMatchTheGame()
{
    // Keys read back from real archives of the base game, with the seventh number the search finds.
    Require(AcdArchive::MakeKey("ks_porsche_718_boxster_s_pdk", 15) == "6-105-61-232-126-93-15-108", "the Boxster's key");
    Require(AcdArchive::MakeKey("abarth500", 21) == "7-248-6-221-246-250-21-49", "a short name's key");
    Require(AcdArchive::MakeKey("bmw_m3_e30", 73) == "108-96-216-121-166-192-73-49", "a BMW's key");
    // Where the fifth number starts from 66 rather than 2 (the two differ only in a high bit).
    Require(AcdArchive::MakeKey("lotus_49", 63) == "3-31-241-236-170-18-63-58", "lotus_49's key");
    Require(AcdArchive::MakeKey("p4-5_2011", 14) == "41-31-5-202-208-57-14-50", "a name with a dash");
}

void AcdArchiveDecryptsAndRefusesAWrongFolder()
{
    const std::map<std::string, std::string> files = BoxsterDataFiles();
    for (const int seventh : {0, 15, 77, 255})
    {
        const AcdArchive::Files read = AcdArchive::Parse(BuildAcd("ks_fixture", seventh, files), "ks_fixture", "fixture");
        Require(read == files, "the archive decrypts to its files, seventh " + std::to_string(seventh));
    }
    // Names are lower-cased; the older layout has no marker.
    const std::map<std::string, std::string> upperNames{
        {"CAR.INI", files.at("car.ini")}, {"power.lut", files.at("power.lut")}, {"engine.ini", files.at("engine.ini")}};
    Require(AcdArchive::Parse(BuildAcd("ks_fixture", 9, upperNames, false), "ks_fixture", "fixture").count("car.ini") == 1,
            "an archive without the marker, and upper-case names");
    // The game keys the archive on its folder's name: as spelled, then in lower case.
    Require(AcdArchive::Parse(BuildAcd("ks_fixture", 9, files), "KS_Fixture", "fixture") == files, "a folder name in another case");
    RequireThrows([&]
                  {
                      AcdArchive::Parse(BuildAcd("ks_fixture", 9, files), "renamed_fixture", "fixture");
                  },
                  "an archive under another folder name");
    std::vector<std::uint8_t> truncated = BuildAcd("ks_fixture", 9, files);
    truncated.resize(truncated.size() - 30);
    RequireThrows([&]
                  {
                      AcdArchive::Parse(truncated, "ks_fixture", "fixture");
                  },
                  "a truncated archive");
    RequireThrows([&]
                  {
                      AcdArchive::Parse(std::vector<std::uint8_t>(12, 0), "ks_fixture", "fixture");
                  },
                  "garbage");
}

void CarDataBecomesASpec()
{
    const VehicleCarSpec spec = AcCarData::BuildSpec(BoxsterDataFiles());
    Require(spec.massKg == 1460.0f, "the mass");
    Require(spec.fuelLitres == 30.0f && spec.fuelTankPosition == glm::vec3(0.0f, -0.15f, -0.85f), "the starting fuel and its tank");
    Require(spec.drive == VehicleDrive::RearWheel, "the drive");
    Require(spec.minRpm == 900.0f && spec.maxRpm == 7500.0f, "the revs");
    Require(spec.gearRatios.size() == 7 && spec.gearRatios[0] == 3.91f && spec.gearRatios[6] == 0.62f, "seven forward gears");
    Require(spec.reverseGearRatio == -3.55f && spec.finalDriveRatio == 3.62f, "reverse and the final drive");
    RequireNear(*spec.maxSteerAngleDegrees, 400.0f / 15.0f, 1e-4f, "the front wheels' lock is the steering wheel's over the ratio");
    Require(spec.steeringWheelLockDegrees == 400.0f, "the steering wheel's lock");
    RequireNear(*spec.brakeTorquePerWheel, 1600.0f, 1e-3f, "each wheel takes MAX_TORQUE times its axle's share: half of it on average");
    Require(spec.frontBrakeShare == 0.65f, "the front's share");
    RequireNear(*spec.handBrakeTorquePerWheel, 2000.0f, 1e-3f, "the hand brake on each rear wheel");
    Require(spec.limitedSlipDifferentials == true && spec.antiRollBars == true, "a locking differential and anti-roll bars");
    // Torque is the file's times one plus the boost: 184 Nm at 2000 rpm, where the turbo is at its
    // wastegate's 1.1, is 386.4; at 500 rpm it has hardly begun (1.2 * (500 / 1900)^2 = 0.083).
    Require(spec.torqueCurve.size() == 10, "a point for each of the file's");
    float peak = 0.0f;
    for (const glm::vec2& point : spec.torqueCurve)
    {
        peak = std::max(peak, point.y);
        if (point.x == 2000.0f)
        {
            RequireNear(point.y, 184.0f * 2.1f, 0.05f, "turbo torque at 2000 rpm");
        }
        if (point.x == 500.0f)
        {
            RequireNear(point.y, 110.0f * (1.0f + 1.2f * (500.0f / 1900.0f) * (500.0f / 1900.0f)), 0.05f, "no boost yet at 500 rpm");
        }
    }
    RequireNear(peak, 386.4f, 0.1f, "the peak");
    // At 1500 rpm the turbo's maximum scaled by the revs, 1.2 * (1500 / 1900)^2 = 0.748, is below its
    // wastegate's 1.1: that is the boost (the wastegate's 1.1 scaled instead would give 0.686).
    for (const glm::vec2& point : spec.torqueCurve)
    {
        if (point.x == 1500.0f)
        {
            RequireNear(point.y, 176.0f * (1.0f + 1.2f * (1500.0f / 1900.0f) * (1500.0f / 1900.0f)), 0.05f, "boost below the wastegate");
        }
    }
    // A turbo whose maximum is far above its wastegate (the Skyline R34's: 1.2 against 0.4, reference
    // 3400 rpm, gamma 2) is at its wastegate from 1963 rpm, well below the reference.
    RequireNear(AcCarData::TurboBoost(2500.0f, 1.2f, 0.4f, 3400.0f, 2.0f), 0.4f, 1e-6f, "at the wastegate below the reference");
    RequireNear(AcCarData::TurboBoost(1500.0f, 1.2f, 0.4f, 3400.0f, 2.0f), 1.2f * (1500.0f / 3400.0f) * (1500.0f / 3400.0f), 1e-6f, "and scaled below that");
    RequireNear(AcCarData::TurboBoost(5000.0f, 1.2f, 0.0f, 3400.0f, 2.0f), 1.2f, 1e-6f, "no wastegate: the maximum");
    RequireNear(AcCarData::TurboBoost(0.0f, 1.2f, 0.4f, 3400.0f, 2.0f), 0.0f, 1e-6f, "nothing at standstill");

    // The ERS: its curves and figures, and both profiles with their controllers. The torque curve above
    // stays the engine's.
    Require(spec.ers.has_value(), "the ERS");
    Require(spec.ers->torqueCurve.size() == 5 && spec.ers->torqueCurve[1] == glm::vec2(4000.0f, 260.0f), "its torque curve");
    Require(spec.ers->coastCurve.size() == 2, "its coast curve");
    Require(spec.ers->chargeK == 0.00014f && spec.ers->dischargeSeconds == 87.0f && spec.ers->maxKjPerLap == 10000000.0f, "its battery");
    Require(spec.ers->hasButtonOverride && spec.ers->brakeRearCorrection == 1.0f && spec.ers->defaultProfile == 0, "its options");
    Require(spec.ers->heatChargeK == 0.002f && spec.ers->heatTorquePercent == 10.0f, "the heat recovery");
    Require(spec.ers->profiles.size() == 2 && spec.ers->profiles[0].name == "Race" && spec.ers->profiles[1].name == "Charging", "both profiles");
    Require(spec.ers->profiles[0].controllers.size() == 2 && spec.ers->profiles[0].controllers[1].input == "GEAR" &&
                spec.ers->profiles[0].controllers[1].combinator == "MULT" && spec.ers->profiles[0].controllers[1].curve.size() == 3 &&
                spec.ers->profiles[0].controllers[0].downLimit == -1.0f && spec.ers->profiles[0].controllers[0].filter == 0.96f,
            "with their controllers");
    Require(!AcCarData::BuildSpec({{"engine.ini", "[ENGINE_DATA]\r\nLIMITER=7000\r\n"}}).ers.has_value(), "no ers.ini, no ERS");
    // The springs' natural frequency on one wheel's sprung mass: sqrt(30760 / (1460 * 0.455 / 2 - 70)) / 2pi
    // = 1.7 Hz at the front, 1.6 at the back; the dampers about 0.7 of critical.
    Require(*spec.suspensionFrequencyHz > 1.5f && *spec.suspensionFrequencyHz < 1.9f, "the springs' frequency");
    Require(*spec.suspensionDamping > 0.5f && *spec.suspensionDamping < 0.9f, "the dampers");

    // Launch and clutch: the inertia, the gearbox's times, the clutch and its profiles.
    Require(spec.engineInertia == 0.137f, "the engine's inertia");
    Require(spec.coastRpm == 7500.0f && spec.coastTorque == 90.0f, "engine braking");
    Require(spec.turbos.size() == 1 && spec.turbos[0].lagUp == 0.99f && spec.turbos[0].referenceRpm == 1900.0f, "the turbo");
    Require(spec.changeUpSeconds == 0.03f && spec.changeDownSeconds == 0.16f && spec.autoCutoffSeconds == 0.035f, "the gearbox's times");
    Require(spec.gearSwitchSeconds == 0.03f, "a change is over in the upshift's time");
    Require(spec.clutchMaxTorque == 700.0f && spec.autoClutchMinRpm == 1200.0f && spec.autoClutchMaxRpm == 1800.0f, "the clutch");
    Require(spec.upshiftClutchProfile.empty() && spec.downshiftClutchProfile.size() == 2 && spec.downshiftClutchProfile[1] == 0.17f,
            "the autoclutch's profiles");
    Require(spec.clutchReleaseSeconds == 0.0f, "the clutch bites as the change ends");
    Require(!spec.autoShiftUpRpm.has_value(), "no [AUTO_SHIFTER], no change point");
    // An upshift profile's points count from the change's start: one longer than the change stretches it.
    {
        std::map<std::string, std::string> files = BoxsterDataFiles();
        files["drivetrain.ini"] = "[TRACTION]\r\nTYPE=RWD\r\n[GEARBOX]\r\nCHANGE_UP_TIME=240\r\n[AUTOCLUTCH]\r\nUPSHIFT_PROFILE=UP_PROFILE\r\n"
                                  "[UP_PROFILE]\r\nPOINT_0=20\r\nPOINT_1=200\r\nPOINT_2=300\r\n[AUTO_SHIFTER]\r\nUP=6900\r\nDOWN=3500\r\n";
        const VehicleCarSpec stretched = AcCarData::BuildSpec(files);
        RequireNear(*stretched.gearSwitchSeconds, 0.3f, 1e-6f, "a 0.3 s profile stretches a 0.24 s change");
        Require(stretched.clutchReleaseSeconds == 0.0f, "and the clutch still bites as it ends");
        Require(stretched.autoShiftUpRpm == 6900.0f && stretched.autoShiftDownRpm == 3500.0f, "the game's automatic gearbox's points");
    }
    Require(spec.differentialPower == 0.25f && spec.differentialCoast == 0.4f && spec.differentialPreload == 5.0f, "the differential");

    // Grip: every compound whole, and the default one's at the load a wheel carries at rest.
    Require(spec.tyreCompounds.size() == 2 && spec.defaultTyreCompound == 0, "two compounds, the first the default");
    const VehicleTyreData& front = spec.tyreCompounds[0].front;
    Require(front.name == "Semislicks" && front.shortName == "SM" && spec.tyreCompounds[1].rear.name == "Street", "the compounds' names");
    Require(front.values.at("DX0") == 1.3114f && front.values.at("FZ0") == 3606.0f && front.values.at("WIDTH") == 0.235f, "every number of the section");
    Require(front.values.at("THERMAL_FRICTION_K") == 0.04767f, "the thermal section's, under its prefix");
    Require(front.curves.at("WEAR_CURVE").size() == 3 && front.curves.at("WEAR_CURVE")[2] == glm::vec2(10000.0f, 0.7f), "the wear curve");
    Require(front.curves.at("THERMAL_PERFORMANCE_CURVE").size() == 3, "the temperature curve");
    Require(front.values.count("NAME") == 0, "text is not a number");
    // 1460 kg with 45.5% on the front axle: 3258.6 N on a front wheel, 3903.7 N on a rear.
    const float frontLoad = 1460.0f * 9.81f * 0.455f * 0.5f;
    const float rearLoad = 1460.0f * 9.81f * 0.545f * 0.5f;
    Require(spec.frontTyres.has_value() && spec.rearTyres.has_value(), "the tyres the physics takes");
    RequireNear(spec.frontTyres->longitudinalGrip, 1.30f * std::pow(frontLoad / 3606.0f, 0.8915f - 1.0f), 1e-3f, "front longitudinal grip at its load");
    RequireNear(spec.frontTyres->lateralGrip, 1.28f * std::pow(frontLoad / 3606.0f, 0.8273f - 1.0f), 1e-3f, "front lateral grip");
    RequireNear(spec.rearTyres->longitudinalGrip, 1.30f * std::pow(rearLoad / 3724.0f, 0.9065f - 1.0f), 1e-3f, "rear longitudinal grip at its load");
    RequireNear(spec.frontTyres->peakSlipAngleDegrees, 7.52f, 1e-4f, "the slip angle at the peak");
    RequireNear(spec.frontTyres->peakSlipRatio, std::tan(7.52f * 3.14159265f / 180.0f), 1e-3f, "and a slip ratio from it");
    Require(spec.frontTyres->postPeakShare == 0.86f && spec.frontTyres->inertia == 1.62f && spec.rearTyres->inertia == 1.97f, "falloff and wheel inertia");
    // Grip near 1.3 is what the real tyres have; the physics engine's own peak is 1.2.
    Require(spec.frontTyres->longitudinalGrip > 1.25f && spec.frontTyres->longitudinalGrip < 1.4f, "a semislick's grip");

    // The air.
    Require(spec.aeroWings.size() == 2 && spec.aeroControllers.size() == 1, "two wings and a controller");
    const VehicleAeroWing& body = spec.aeroWings[0];
    Require(body.name == "BODY" && body.chord == 1.0f && body.span == 1.99f && body.position == glm::vec3(0.0f, 0.15f, -0.10f), "the body's wing");
    Require(body.dragCurve.size() == 4 && body.dragCurve[1] == glm::vec2(0.0f, 0.39f) && body.liftCurve.size() == 4, "and its curves");
    Require(body.values.at("ZONE_FRONT_CD") == 0.005f && body.curves.count("LUT_GH_CL") == 0, "the zone modifier, and no curve for an empty file name");
    Require(spec.aeroWings[1].position == glm::vec3(0.0f, 0.34f, -1.6f) && spec.aeroWings[1].angleDegrees == 2.0f, "a position with a space in it, and an angle");
    const VehicleAeroController& controller = spec.aeroControllers[0];
    Require(controller.wing == 1 && controller.input == "SPEED_KMH" && controller.curve.size() == 4 && controller.upLimit == 10.0f, "the controller");
    Require(spec.electronics.at("TRACTION_CONTROL").at("SLIP_RATIO_LIMIT") == 0.08f && spec.electronics.at("ABS").at("PRESENT") == 1.0f &&
                spec.electronics.at("TRACTION_CONTROL").count("CURVE") == 0,
            "the driver aids' numbers");

    // Missing files leave their fields out instead of making them up.
    const VehicleCarSpec bare = AcCarData::BuildSpec({{"car.ini", "[BASIC]\nTOTALMASS=900\n"}});
    Require(bare.massKg == 900.0f && !bare.drive.has_value() && bare.torqueCurve.empty() && bare.gearRatios.empty() &&
                !bare.suspensionFrequencyHz.has_value() && !bare.brakeTorquePerWheel.has_value(),
            "only what the files say");
    // An open differential and a car with no ARB section.
    const VehicleCarSpec open = AcCarData::BuildSpec({{"drivetrain.ini", "[TRACTION]\nTYPE=AWD2\n[DIFFERENTIAL]\nPOWER=0\nCOAST=0\n"}});
    Require(open.drive == VehicleDrive::AllWheel && open.limitedSlipDifferentials == false, "AWD2 is all-wheel, and an open differential locks nothing");
    Require(!open.antiRollBars.has_value(), "no ARB section, no opinion");

    const AcCarData::Ini ini = AcCarData::ParseIni("[a b]\r\nkey = 1 ; x\r\n; whole line\r\nno equals\r\nK2=v=w\r\n");
    Require(ini.at("A B").at("KEY") == "1" && ini.at("A B").at("K2") == "v=w" && ini.at("A B").size() == 2, "the ini parser");
    const auto lut = AcCarData::ParseLut("0|1\r\n; c\r\n5|2.5 ; tail\r\nbad|line\r\n");
    Require(lut.size() == 2 && lut[1].first == 5.0f && lut[1].second == 2.5f, "the lut parser");
}

// Every car of an Assetto Corsa install (MINIENGINE_AC_CARS = its content/cars folder) opens with the key
// the folder name gives, and reads as a car. Skipped without the variable: it needs the game.
void EveryInstalledCarDecrypts()
{
    const char* folder = std::getenv("MINIENGINE_AC_CARS");
    if (folder == nullptr)
    {
        return;
    }
    size_t archives = 0;
    std::vector<std::string> failures;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(folder))
    {
        if (!entry.is_directory() || !std::filesystem::exists(entry.path() / "data.acd"))
        {
            continue;
        }
        ++archives;
        std::string problem;
        const std::optional<VehicleCarSpec> spec = AcCarData::ReadCarFolder(entry.path(), &problem);
        if (!spec.has_value() || !spec->massKg.has_value() || spec->torqueCurve.empty() || spec->gearRatios.empty() || !spec->drive.has_value() ||
            spec->tyreCompounds.empty() || !spec->frontTyres.has_value() || !spec->rearTyres.has_value() || spec->aeroWings.empty() ||
            !spec->engineInertia.has_value())
        {
            failures.push_back(entry.path().filename().string() + (problem.empty() ? " (incomplete)" : " (" + problem + ")"));
        }
    }
    std::cout << archives << " installed cars read, " << failures.size() << " failed\n";
    for (const std::string& failure : failures)
    {
        std::cout << "  " << failure << '\n';
    }
    Require(archives > 0 && failures.empty(), "every installed car reads");
}

// The Boxster's data with a live rear axle as the AE86 has one: TYPE=AXLE and an [AXLE] section of five
// links. The spec has them in the axle's frame (forward, left, up), and they survive the glTF.
std::map<std::string, std::string> LiveAxleDataFiles()
{
    std::map<std::string, std::string> files = BoxsterDataFiles();
    std::string& suspension = files.at("suspensions.ini");
    const std::string strut = "[REAR]\r\nTYPE=STRUT\r\n";
    suspension.replace(suspension.find(strut), strut.size(), "[REAR]\r\nTYPE=AXLE\r\n");
    suspension +=
        "\r\n[AXLE]\r\nLINK_COUNT=5\r\n"
        "J0_CAR=0.4933,-0.020,0.498 ; car bottom left arm\r\nJ0_AXLE=0.4900,-0.080,0.0\r\n"
        "J1_CAR=-0.4933,-0.020,0.498\r\nJ1_AXLE=-0.4900,-0.080,0.0\r\n"
        "J2_CAR=0.2488,0.075,0.2405\r\nJ2_AXLE=0.2488,0.020,0.0\r\n"
        "J3_CAR=-0.2488,0.075,0.2405\r\nJ3_AXLE=-0.2488,0.020,0.0\r\n"
        "J4_AXLE=0.435,-0.070,-0.110\r\nJ4_CAR=-0.435,0.010,-0.100\r\n"
        "TORQUE_REACTION=-0.5\r\nATTACH_REL_POS=0.72\r\nLEAF_SPRING_LAT_K=0\r\n";
    return files;
}

void LiveAxleDataBecomesASolidAxle()
{
    const VehicleCarSpec spec = AcCarData::BuildSpec(LiveAxleDataFiles());
    Require(spec.rearSuspension.has_value() && spec.rearSuspension->type == VehicleSuspensionType::SolidAxle, "the rear is a solid axle");
    const VehicleSuspensionAxle& rear = *spec.rearSuspension;
    Require(rear.axleLinks.size() == 5, "with its five links");
    // J0: the car end (x left 0.4933, y up -0.02, z forward 0.498) as (forward, left, up).
    Require(rear.axleLinks[0].chassis == glm::vec3(0.498f, 0.4933f, -0.020f) && rear.axleLinks[0].axle == glm::vec3(0.0f, 0.49f, -0.08f), "the first link's ends");
    Require(rear.axleLinks[4].chassis == glm::vec3(-0.100f, -0.435f, 0.010f) && rear.axleLinks[4].axle == glm::vec3(-0.110f, 0.435f, -0.070f),
            "the Panhard rod across the car");
    Require(rear.axleSpringPosition == 0.72f && rear.axleTorqueReaction == -0.5f && rear.axleLateralStiffness == 0.0f, "where the springs sit and the rest");
    Require(rear.wheelRate == 42500.0f && rear.hubMass == 80.0f, "its springs and hub as the axle's");

    // Through the import's glTF and back.
    ScopedDirectory scope;
    const std::filesystem::path kn5 = WriteCarFolder(scope.Path());
    WriteFile(kn5.parent_path() / "data.acd", BuildAcd("ks_fixture", 42, LiveAxleDataFiles()));
    const Kn5ImportReport report = Kn5Importer::ConvertToGltf(kn5, scope.Path() / "out");
    const LoadedModelData model = ModelLoader::LoadModel(report.gltfPath.string());
    Require(model.carSpec.has_value() && model.carSpec->rearSuspension.has_value(), "the model carries the axle");
    const VehicleSuspensionAxle& loaded = *model.carSpec->rearSuspension;
    Require(loaded.type == VehicleSuspensionType::SolidAxle && loaded.axleLinks == rear.axleLinks && loaded.axleSpringPosition == rear.axleSpringPosition &&
                loaded.axleTorqueReaction == rear.axleTorqueReaction,
            "the solid axle survives the glTF");
}

// The Skyline R34's drive, rear steering and body, as its data.acd has them (shortened), on the Boxster's
// other files: ATTESA as AWD2 (with the [AWD] section it does not drive on), Super HICAS with inline curves,
// three floor boxes, and a centre controller with a lut file as the AWD2 cars that have one carry it.
std::map<std::string, std::string> R34DataFiles()
{
    std::map<std::string, std::string> files = BoxsterDataFiles();
    files["drivetrain.ini"] =
        "[TRACTION]\r\nTYPE=AWD2 ; Wheel drive\r\n[GEARS]\r\nCOUNT=6\r\nGEAR_R=-3.280\r\nGEAR_1=3.827\r\nGEAR_2=2.360\r\nGEAR_3=1.685\r\nGEAR_4=1.312\r\n"
        "GEAR_5=1.00\r\nGEAR_6=0.793\r\nFINAL=3.545\r\n[DIFFERENTIAL]\r\nPOWER=0.50\r\nCOAST=0.50\r\nPRELOAD=0\r\n"
        "[AWD]\r\nFRONT_SHARE=1\r\nFRONT_DIFF_POWER=0.06\r\nCENTRE_DIFF_PRELOAD=1\r\nREAR_DIFF_POWER=0.525\r\n"
        "[AWD2]\r\nFRONT_DIFF_POWER=0.03\r\nFRONT_DIFF_COAST=0.03\r\nFRONT_DIFF_PRELOAD=0\r\nCENTRE_RAMP_TORQUE=100.0\r\nCENTRE_MAX_TORQUE=1000.0\r\n"
        "REAR_DIFF_POWER=0.60\r\nREAR_DIFF_COAST=0.50\r\nREAR_DIFF_PRELOAD=10\r\n[AUTO_SHIFTER]\r\nUP=7900\r\nDOWN=4200\r\n";
    files["ctrl_4ws.ini"] =
        "[CONTROLLER_0]\r\nINPUT=STEER_DEG  ; OVERSTEER_FACTOR REAR_SPEED_RATIO\r\nCOMBINATOR=ADD\r\nLUT=(|-90=-0.0015|-25=-0.0010|-10=0.0|0=0|10=0.0|25=0.0010|90=0.0015|)\r\n"
        "FILTER=0.99\r\nUP_LIMIT=1\r\nDOWN_LIMIT=-1\r\n"
        "[CONTROLLER_1]\r\nINPUT=OVERSTEER_FACTOR\r\nCOMBINATOR=MULT\r\nLUT=(|-1.6=-2|-1.2=1|0=1|1.2=1|1.5=2|)\r\nFILTER=0.99\r\nUP_LIMIT=1\r\nDOWN_LIMIT=-1\r\n"
        "[CONTROLLER_2]\r\nINPUT=SPEED_KMH\r\nCOMBINATOR=MULT\r\nLUT=(|0=1|130=1|150=0.2|)\r\nFILTER=0.99\r\nUP_LIMIT=1\r\nDOWN_LIMIT=-1\r\n";
    files["colliders.ini"] = "[COLLIDER_0]\r\nCENTRE=0 ,-0.23 ,-0.9\r\nSIZE=1.75 ,0.15 ,3.0\r\nGROUND_ENABLE=1\r\n"
                             "[COLLIDER_1]\r\nCENTRE=0 ,-0.26 ,1.1\r\nSIZE=1.75 ,0.15 ,1.0\r\nGROUND_ENABLE=1\r\n"
                             "[COLLIDER_2]\r\nCENTRE=0 ,-0.38 ,1.8\r\nSIZE=1.57 ,0.15 ,0.35\r\nGROUND_ENABLE=0\r\n";
    files["ctrl_awd2.ini"] = "[CONTROLLER_0]\r\nINPUT=GEAR\r\nCOMBINATOR=ADD\r\nLUT=gear_start.lut\r\nFILTER=0.99\r\nUP_LIMIT=1000000\r\nDOWN_LIMIT=0.0\r\n";
    files["gear_start.lut"] = "0|0\r\n1|550\r\n2|450\r\n";
    return files;
}

void FourWheelDriveRearSteerAndBodyBecomeASpec()
{
    const VehicleCarSpec spec = AcCarData::BuildSpec(R34DataFiles());
    Require(spec.drive == VehicleDrive::AllWheel && spec.allWheelDrive.has_value(), "four-wheel drive");
    const VehicleAllWheelDrive& awd = *spec.allWheelDrive;
    Require(awd.coupling, "a coupling, as TYPE=AWD2 says");
    Require(awd.centreRampTorque == 100.0f && awd.centreMaxTorque == 1000.0f, "its ramp and limit from [AWD2]");
    Require(awd.frontDiffPower == 0.03f && awd.frontDiffCoast == 0.03f && awd.frontDiffPreload == 0.0f, "the front differential from [AWD2], not [AWD]");
    Require(awd.rearDiffPower == 0.6f && awd.rearDiffCoast == 0.5f && awd.rearDiffPreload == 10.0f, "the rear differential");
    Require(awd.centreControllers.size() == 1 && awd.centreControllers[0].curve.size() == 3 && awd.centreControllers[0].curve[1] == glm::vec2(1.0f, 550.0f) &&
                awd.centreControllers[0].upLimit == 1000000.0f,
            "the centre's controller with its lut file");

    Require(spec.rearSteerControllers.size() == 3, "three rear steering controllers");
    const VehicleController& steer = spec.rearSteerControllers[0];
    Require(steer.input == "STEER_DEG" && steer.combinator == "ADD" && steer.filter == 0.99f && steer.upLimit == 1.0f && steer.downLimit == -1.0f,
            "the first reads the steering wheel");
    Require(steer.curve.size() == 7 && steer.curve.front() == glm::vec2(-90.0f, -0.0015f) && steer.curve[5] == glm::vec2(25.0f, 0.0010f), "its inline curve");
    Require(spec.rearSteerControllers[1].input == "OVERSTEER_FACTOR" && spec.rearSteerControllers[1].combinator == "MULT" &&
                spec.rearSteerControllers[1].curve.size() == 5,
            "the second the oversteer");
    Require(spec.rearSteerControllers[2].curve.size() == 3 && spec.rearSteerControllers[2].curve[2] == glm::vec2(150.0f, 0.2f), "the third the speed");

    Require(spec.colliders.size() == 3, "three boxes");
    Require(spec.colliders[0].center == glm::vec3(0.0f, -0.23f, -0.9f) && spec.colliders[0].size == glm::vec3(1.75f, 0.15f, 3.0f) && spec.colliders[0].groundEnabled,
            "the floor box");
    Require(spec.colliders[2].size == glm::vec3(1.57f, 0.15f, 0.35f) && !spec.colliders[2].groundEnabled, "and one kept off the ground");

    // A plain AWD reads its centre differential's share from [AWD]; a rear-drive car has none of it.
    std::map<std::string, std::string> awdFiles = R34DataFiles();
    awdFiles["drivetrain.ini"] = "[TRACTION]\r\nTYPE=AWD\r\n[AWD]\r\nFRONT_SHARE=0.35\r\nCENTRE_DIFF_POWER=0.2\r\n";
    const VehicleCarSpec centre = AcCarData::BuildSpec(awdFiles);
    Require(centre.allWheelDrive.has_value() && !centre.allWheelDrive->coupling && centre.allWheelDrive->frontShare == 0.35f &&
                centre.allWheelDrive->centreDiffPower == 0.2f && centre.allWheelDrive->centreControllers.empty(),
            "a centre differential's share");
    Require(!AcCarData::BuildSpec(BoxsterDataFiles()).allWheelDrive.has_value(), "a rear-drive car has no four-wheel-drive figures");
}

// The R34's figures through the import and back, with its collider.kn5 (here the fixture car's own kn5).
void ImportWritesFourWheelDriveRearSteerAndBody()
{
    ScopedDirectory scope;
    const std::filesystem::path kn5 = WriteCarFolder(scope.Path());
    const std::filesystem::path carFolder = kn5.parent_path();
    WriteFile(carFolder / "data.acd", BuildAcd("ks_fixture", 42, R34DataFiles()));
    WriteFile(carFolder / "collider.kn5", BuildCarKn5(6));
    size_t shellPoints = 0;
    std::function<void(const Kn5Node&)> count = [&](const Kn5Node& node)
    {
        shellPoints += node.vertices.size();
        for (const Kn5Node& child : node.children)
        {
            count(child);
        }
    };
    count(Kn5Reader::Load(carFolder / "collider.kn5").root);

    const VehicleCarSpec expected = AcCarData::BuildSpec(R34DataFiles());
    const std::optional<VehicleCarSpec> folder = AcCarData::ReadCarFolder(carFolder);
    Require(folder.has_value() && folder->colliderHull.size() == shellPoints && shellPoints > 0, "the car's folder gives the shell's points");

    const Kn5ImportReport report = Kn5Importer::ConvertToGltf(kn5, scope.Path() / "r34");
    const LoadedModelData model = ModelLoader::LoadModel(report.gltfPath.string());
    Require(model.carSpec.has_value(), "the model carries the figures");
    const VehicleCarSpec& spec = *model.carSpec;
    Require(spec.allWheelDrive.has_value() && spec.allWheelDrive->coupling && spec.allWheelDrive->centreRampTorque == 100.0f &&
                spec.allWheelDrive->centreMaxTorque == 1000.0f && spec.allWheelDrive->rearDiffPreload == 10.0f && spec.allWheelDrive->frontDiffPower == 0.03f,
            "the four-wheel drive survives the glTF");
    Require(spec.allWheelDrive->centreControllers.size() == 1 && spec.allWheelDrive->centreControllers[0].upLimit == 1000000.0f, "with its centre's controller");
    Require(spec.rearSteerControllers.size() == 3 && spec.rearSteerControllers[0].curve == expected.rearSteerControllers[0].curve &&
                spec.rearSteerControllers[1].combinator == "MULT" && spec.rearSteerControllers[2].filter == 0.99f,
            "the rear steering survives");
    Require(spec.colliders == expected.colliders, "the boxes survive");
    Require(spec.autoShiftUpRpm == 7900.0f && spec.autoShiftDownRpm == 4200.0f, "the automatic gearbox's points survive");
    Require(spec.colliderHull.size() == shellPoints, "the shell survives");
    for (size_t index = 0; index < shellPoints; ++index)
    {
        RequireNear(glm::length(spec.colliderHull[index] - folder->colliderHull[index]), 0.0f, 1e-4f, "each of its points");
    }
}

void ImportWritesTheCarsOwnData()
{
    ScopedDirectory scope;
    const std::filesystem::path kn5 = WriteCarFolder(scope.Path());
    const std::filesystem::path carFolder = kn5.parent_path();

    // Without data next to the kn5 the import says nothing about the car's figures.
    {
        const Kn5ImportReport plain = Kn5Importer::ConvertToGltf(kn5, scope.Path() / "plain");
        Require(plain.carData.empty() && plain.carDataProblem.empty(), "no data, no figures");
        Require(!ModelLoader::LoadModel(plain.gltfPath.string()).carSpec.has_value(), "the model has no car data");
    }

    // A data.acd next to it: the figures reach the loaded model.
    {
        const std::vector<std::uint8_t> archive = BuildAcd("ks_fixture", 42, BoxsterDataFiles());
        WriteFile(carFolder / "data.acd", archive);
        const Kn5ImportReport report = Kn5Importer::ConvertToGltf(kn5, scope.Path() / "with_data");
        Require(report.carData.find("1460 kg") != std::string::npos && report.carData.find("RWD") != std::string::npos,
                "the report names the figures: " + report.carData);
        const LoadedModelData model = ModelLoader::LoadModel(report.gltfPath.string());
        Require(model.carSpec.has_value(), "the model carries the car's figures");
        const VehicleCarSpec& spec = *model.carSpec;
        Require(spec.massKg == 1460.0f && spec.drive == VehicleDrive::RearWheel && spec.gearRatios.size() == 7,
                "mass, drive and gears survive the glTF");
        Require(spec.torqueCurve.size() == 10 && spec.maxRpm == 7500.0f && spec.finalDriveRatio == 3.62f, "the torque curve and revs survive");
        Require(spec.limitedSlipDifferentials == true && spec.antiRollBars == true, "and the flags");
        Require(spec.fuelLitres == 30.0f && spec.fuelTankPosition == glm::vec3(0.0f, -0.15f, -0.85f), "and the fuel");
        Require(spec.cockpitCamera.has_value() && spec.cockpitCamera->position == glm::vec3(-0.35f, 1.05f, 0.3f) &&
                    spec.cockpitCamera->pitchDegrees == -4.0f,
                "and the driver's eyes, in the model's frame");
        RequireNear(*spec.suspensionFrequencyHz, *AcCarData::BuildSpec(BoxsterDataFiles()).suspensionFrequencyHz, 1e-3f, "and the springs");
        // Everything else the data holds comes through too.
        const VehicleCarSpec expected = AcCarData::BuildSpec(BoxsterDataFiles());
        Require(spec.engineInertia == expected.engineInertia && spec.gearSwitchSeconds == expected.gearSwitchSeconds &&
                    spec.clutchReleaseSeconds == expected.clutchReleaseSeconds && spec.clutchMaxTorque == 700.0f,
                "launch and clutch survive");
        Require(spec.frontTyres.has_value() && spec.frontTyres->postPeakShare == 0.86f && spec.frontTyres->inertia == 1.62f, "the tyres survive");
        // The suspension linkage: AC's (towards the centre, up, forward) as (forward, outward, up).
        Require(spec.frontSuspension.has_value() && spec.rearSuspension.has_value(), "both axles' linkage survives");
        Require(spec.frontSuspension->type == VehicleSuspensionType::MacPherson && spec.rearSuspension->type == VehicleSuspensionType::MacPherson, "as struts");
        RequireNear(spec.frontSuspension->strutTop.x, -0.08294f, 1e-5f, "the top mount's forward offset");
        RequireNear(spec.frontSuspension->strutTop.y, -0.28497f, 1e-5f, "inboard of the wheel");
        RequireNear(spec.frontSuspension->strutTop.z, 0.40218f, 1e-5f, "and above it");
        RequireNear(spec.frontSuspension->tieOuter.x, 0.14781f, 1e-5f, "the tie rod ahead of the axle");
        Require(spec.frontSuspension->wheelRate == 30760.0f && spec.frontSuspension->dampFastRebound == 2601.0f && spec.frontSuspension->bumpStopTravel == 0.08f,
                "the wheel rate, dampers and bump stops");
        RequireNear(spec.frontSuspension->staticCamberDegrees, -1.6f, 1e-5f, "the static camber");
        RequireNear(spec.frontSuspension->toeOutRodLength, -0.0003f, 1e-7f, "the toe rod");
        Require(spec.frontSuspension->rodLength.has_value() && std::abs(*spec.frontSuspension->rodLength - 0.045f) < 1e-6f, "the front's ROD_LENGTH");
        Require(!spec.rearSuspension->rodLength.has_value(), "the rear has none");
        Require(spec.frontSuspension->packerRange.has_value() && std::abs(*spec.frontSuspension->packerRange - 0.09f) < 1e-6f, "the front's PACKER_RANGE");
        Require(!spec.rearSuspension->packerRange.has_value(), "the rear has no packers");
        Require(spec.frontSuspension->antiRollBarRate == 30000.0f && spec.rearSuspension->antiRollBarRate == 16000.0f, "the anti-roll bars");
        RequireNear(spec.frontSuspension->centerOfMassAboveWheel, 0.105f, 1e-5f, "BASEY -0.105: the centre of mass above the wheel centre");
        Require(spec.wheelbase.has_value() && *spec.wheelbase == 2.475f && spec.frontWeightShare.has_value(), "the wheelbase and weight split");
        Require(spec.steeringWheelLockDegrees == 400.0f && spec.maxSteerAngleDegrees.has_value(), "the steering lock");
        RequireNear(spec.rearTyres->longitudinalGrip, expected.rearTyres->longitudinalGrip, 1e-3f, "with their grip");
        Require(spec.tyreCompounds.size() == 2 && spec.defaultTyreCompound == 0, "both compounds survive");
        Require(spec.tyreCompounds[0].front.values.at("DX0") == 1.3114f && spec.tyreCompounds[1].front.name == "Street", "with their numbers");
        Require(spec.tyreCompounds[0].rear.curves.at("WEAR_CURVE").size() == 2 && spec.tyreCompounds[0].front.curves.at("THERMAL_PERFORMANCE_CURVE").size() == 3,
                "and their curves");
        Require(spec.aeroWings.size() == 2 && spec.aeroWings[1].position == glm::vec3(0.0f, 0.34f, -1.6f) && spec.aeroWings[0].dragCurve.size() == 4,
                "the wings survive");
        Require(spec.aeroControllers.size() == 1 && spec.aeroControllers[0].input == "SPEED_KMH", "and the controller");
        Require(spec.turbos.size() == 1 && spec.turbos[0].gamma == 2.0f && spec.coastTorque == 90.0f, "the turbo and engine braking survive");
        Require(spec.downshiftClutchProfile.size() == 2 && spec.differentialPreload == 5.0f, "the profiles and the differential survive");
        Require(spec.electronics.at("TRACTION_CONTROL").at("MIN_SPEED_KMH") == 30.0f, "the driver aids survive");
        Require(spec.ers.has_value() && spec.ers->torqueCurve == expected.ers->torqueCurve && spec.ers->coastCurve.size() == 2, "the ERS survives");
        Require(spec.ers->chargeK == expected.ers->chargeK && spec.ers->dischargeSeconds == 87.0f && spec.ers->maxKjPerLap == 10000000.0f &&
                    spec.ers->hasButtonOverride && spec.ers->heatTorquePercent == 10.0f,
                "with its figures");
        Require(spec.ers->profiles.size() == 2 && spec.ers->profiles[0].controllers.size() == 2 &&
                    spec.ers->profiles[0].controllers[1].curve == expected.ers->profiles[0].controllers[1].curve &&
                    spec.ers->profiles[1].name == "Charging",
                "and its profiles");
        Require(report.carData.find("ERS 260 Nm") != std::string::npos, "the report names the ERS: " + report.carData);
    }

    // An archive that will not decrypt (here, one made for another folder) does not stop the import.
    WriteFile(carFolder / "data.acd", BuildAcd("some_other_car", 42, BoxsterDataFiles()));
    {
        const Kn5ImportReport report = Kn5Importer::ConvertToGltf(kn5, scope.Path() / "wrong_folder");
        Require(report.carData.empty() && !report.carDataProblem.empty(), "the problem is reported");
        Require(!ModelLoader::LoadModel(report.gltfPath.string()).carSpec.has_value(), "and the model has no car data");
    }

    // An unpacked data/ folder (as mod authors work in) reads the same.
    std::filesystem::remove(carFolder / "data.acd");
    for (const auto& [name, text] : BoxsterDataFiles())
    {
        WriteFile(carFolder / "data" / name, std::vector<std::uint8_t>(text.begin(), text.end()));
    }
    {
        const Kn5ImportReport report = Kn5Importer::ConvertToGltf(kn5, scope.Path() / "unpacked");
        Require(ModelLoader::LoadModel(report.gltfPath.string()).carSpec->massKg == 1460.0f, "an unpacked data folder");
    }
}

void ImportsMultilayerSurfacesAsDetailLayers()
{
    ScopedDirectory scope;
    const std::filesystem::path kn5 = scope.Path() / "surfaces" / "surfaces.kn5";
    WriteFile(kn5, BuildSurfacesKn5());
    const Kn5ImportReport report = Kn5Importer::ConvertToGltf(kn5, scope.Path() / "assets" / "surfaces");
    const LoadedModelData model = ModelLoader::LoadModel(report.gltfPath.string());
    const auto named = [&model](const std::string& name) -> const ModelMaterialData&
    {
        for (const ModelMaterialData& material : model.materials)
        {
            if (material.name == name)
            {
                return material;
            }
        }
        throw std::runtime_error("no material " + name);
    };

    const ModelMaterialData& asph = named("asph");
    const MaterialDetailLayers& tarmac = asph.detailLayers;
    Require(tarmac.IsEnabled() && tarmac.mapping == DetailLayerMapping::PositionXZ, "ksMultilayer maps its details by world position");
    Require(!tarmac.maskTexturePath.empty() && tarmac.maskTexturePath != asph.baseColorTexturePath, "the mask is its own map");
    Require(!tarmac.layerTexturePaths[0].empty() && tarmac.layerTexturePaths[1] != tarmac.layerTexturePaths[0], "R and G differ");
    Require(tarmac.layerTexturePaths[2] == tarmac.layerTexturePaths[0], "B shares R's map");
    // The model's space is AC's world turned half about Y: x and z both change sign.
    RequireNear(tarmac.layerScales[0][0], -0.8f, 1e-6f, "multR, negated for the half turn");
    RequireNear(tarmac.layerScales[0][1], -0.8f, 1e-6f, "on both axes");
    RequireNear(tarmac.layerScales[1][0], -0.25f, 1e-6f, "multG");
    RequireNear(tarmac.intensity, 1.2f, 1e-6f, "magicMult is the intensity");
    Require(!asph.normalTexturePath.empty(), "txDetailNM is the normal map");
    RequireNear(asph.textureTransforms[static_cast<size_t>(MaterialTextureSlot::Normal)].scale[0], 5.0f, 1e-6f,
                "tiled by detailNMMult");

    const MaterialDetailLayers& kerb = named("kerb").detailLayers;
    Require(kerb.IsEnabled() && kerb.mapping == DetailLayerMapping::TexCoord, "ksMultilayer_objsp maps its details by UV");
    RequireNear(kerb.layerScales[0][0], 12.0f, 1e-6f, "a UV scale is not negated");
    RequireNear(kerb.intensity, 1.0f, 1e-6f, "magicMult defaults to 1");
    Require(named("kerb").normalTexturePath.empty(), "no detail normal without txDetailNM");

    Require(!named("wall").detailLayers.IsEnabled(), "a plain material has no detail layers");

    // Ground is never a tight lobe: exponent 15 is roughness 0.586, raised to the multilayer floor;
    // the sheen multiplier does not sharpen it. A plain material keeps its own (0.549 for the
    // wall's default exponent 20).
    RequireNear(named("glossy_tarmac").roughnessFactor, 0.7f, 1e-6f, "multilayer roughness has a floor");
    RequireNear(named("glossy_tarmac").specularFactor, 1.0f, 1e-6f, "fresnelMaxLevel still caps the specular at 1");
    RequireNear(named("wall").roughnessFactor, 0.54912f, 1e-4f, "the floor is for multilayer surfaces only");
    RequireNear(named("wall").specularFactor, 0.2f, 1e-6f, "ksSpecular without a reflection is the specular level");
    RequireNear(named("asph").roughnessFactor, 0.7f, 1e-6f, "the default exponent is raised to the floor too");
    RequireNear(named("asph").specularFactor, 0.0f, 0.0f, "ksSpecular 0 reflects nothing");
}

// A cockpit's ksPerPixelMultiMap materials: leather on an alpha-0 diffuse (alpha-tested, as the
// Skyline's is), carbon on a diffuse whose alpha is 0 in one half, a grille that really is a
// cutout, and a panel whose detail is a flat paint colour.
std::vector<std::uint8_t> BuildCockpitKn5()
{
    ByteWriter writer;
    writer.Raw("sc6969", 6);
    writer.U32(5);
    std::vector<std::array<std::uint8_t, 4>> checker;
    std::vector<std::array<std::uint8_t, 4>> halfAlpha;
    for (int texel = 0; texel < 16; ++texel)
    {
        const bool odd = ((texel % 4) + (texel / 4)) % 2 == 1;
        checker.push_back(odd ? std::array<std::uint8_t, 4>{200, 180, 160, 255} : std::array<std::uint8_t, 4>{40, 40, 40, 255});
        halfAlpha.push_back({120, 120, 120, static_cast<std::uint8_t>(texel < 8 ? 0 : 255)});
    }
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>> textures{
        {"cockpit.dds", DdsFlat(4, {120, 120, 120, 0})},
        {"skin.dds", DdsBgra(4, 4, halfAlpha)},
        {"flat_nm.dds", DdsFlat(4, {128, 128, 255, 255})},
        {"leather.dds", DdsBgra(4, 4, checker)},
        {"leather_nm.dds", DdsBgra(4, 4, checker)},
        {"carbon.dds", DdsBgra(4, 4, checker)},
        {"grille.dds", DdsBgra(4, 4, halfAlpha)},
        {"paint.dds", DdsFlat(4, {100, 20, 20, 255})},
    };
    writer.U32(static_cast<std::uint32_t>(textures.size()));
    for (const auto& [name, blob] : textures)
    {
        writer.U32(1);
        writer.String(name);
        writer.Blob(blob);
    }
    const std::vector<FixtureMaterial> materials{
        {"Leather", "ksPerPixelMultiMap_AT_NMDetail", false, true,
         {{"useDetail", 1.0f}, {"detailUVMultiplier", 37.0f}, {"detailNormalBlend", 0.7f}},
         {{"txDiffuse", "cockpit.dds"}, {"txNormal", "flat_nm.dds"}, {"txDetail", "leather.dds"}, {"txNormalDetail", "leather_nm.dds"}}},
        {"Carbon", "ksPerPixelMultiMap_NMDetail", false, false,
         {{"useDetail", 1.0f}, {"detailUVMultiplier", 400.0f}, {"detailNormalBlend", 0.0f}},
         {{"txDiffuse", "skin.dds"}, {"txNormal", "flat_nm.dds"}, {"txDetail", "carbon.dds"}, {"txNormalDetail", "leather_nm.dds"}}},
        {"Grille", "ksPerPixelAT", false, true, {}, {{"txDiffuse", "grille.dds"}}},
        {"Panel", "ksPerPixelMultiMap", false, false,
         {{"useDetail", 1.0f}, {"detailUVMultiplier", 5.0f}},
         {{"txDiffuse", "cockpit.dds"}, {"txDetail", "paint.dds"}}},
    };
    writer.U32(static_cast<std::uint32_t>(materials.size()));
    for (const FixtureMaterial& material : materials)
    {
        WriteMaterial(writer, material);
    }
    WriteDummy(writer, "ROOT", 4, kIdentity);
    WriteMesh(writer, "SEAT", 0, 1.0f);
    WriteMesh(writer, "TRIM", 1, 2.0f);
    WriteMesh(writer, "GRILLE", 2, 3.0f);
    WriteMesh(writer, "PANEL", 3, 4.0f);
    return writer.Bytes();
}

void ImportsMultiMapDetailAsDetailLayers()
{
    ScopedDirectory scope;
    const std::filesystem::path kn5 = scope.Path() / "cockpit" / "cockpit.kn5";
    WriteFile(kn5, BuildCockpitKn5());
    const Kn5ImportReport report = Kn5Importer::ConvertToGltf(kn5, scope.Path() / "assets" / "cockpit");
    const LoadedModelData model = ModelLoader::LoadModel(report.gltfPath.string());
    const auto named = [&model](const std::string& name) -> const ModelMaterialData&
    {
        for (const ModelMaterialData& material : model.materials)
        {
            if (material.name == name)
            {
                return material;
            }
        }
        throw std::runtime_error("no material " + name);
    };

    const ModelMaterialData& leather = named("Leather");
    Require(leather.alphaMode == MaterialAlphaMode::Opaque, "an alpha test that would discard every texel is dropped");
    const MaterialDetailLayers& grain = leather.detailLayers;
    Require(grain.IsEnabled() && grain.mapping == DetailLayerMapping::TexCoord, "the detail tiles by UV");
    Require(!grain.layerTexturePaths[0].empty(), "txDetail is the first layer");
    Require(grain.layerTexturePaths[1].empty(), "an alpha-0 diffuse needs no neutral layer");
    RequireNear(grain.layerScales[0][0], 37.0f, 1e-6f, "tiled by detailUVMultiplier");
    RequireNear(grain.intensity, 2.0f, 1e-6f, "doubled: a detail map is neutral at mid-grey");
    int width = 0;
    int height = 0;
    const std::filesystem::path folder = report.gltfPath.parent_path();
    std::vector<std::uint8_t> mask = ReadPngRgba((folder / grain.maskTexturePath).string(), width, height);
    Require(width == 1 && height == 1, "a uniform alpha makes a one-texel mask");
    Require(mask[0] == 255 && mask[1] == 0 && mask[2] == 0 && mask[3] == 0, "all detail, nothing else");
    Require(leather.normalTexturePath.find("leather_nm") != std::string::npos, "txNormalDetail replaces the flat normal");
    RequireNear(leather.normalScale, 0.7f, 1e-6f, "scaled by detailNormalBlend");
    RequireNear(leather.textureTransforms[static_cast<size_t>(MaterialTextureSlot::Normal)].scale[0], 37.0f, 1e-6f,
                "and tiled like the detail");

    const ModelMaterialData& carbon = named("Carbon");
    Require(carbon.detailLayers.IsEnabled() && !carbon.detailLayers.layerTexturePaths[1].empty(),
            "where the diffuse's alpha is 1 a mid-grey layer keeps it as it is");
    mask = ReadPngRgba((folder / carbon.detailLayers.maskTexturePath).string(), width, height);
    Require(width == 4 && height == 4, "a varying alpha makes a full-size mask");
    Require(mask[0] == 255 && mask[1] == 0, "alpha 0: the detail");
    Require(mask[15 * 4] == 0 && mask[15 * 4 + 1] == 255 && mask[15 * 4 + 3] == 0, "alpha 1: the neutral layer");
    Require(carbon.normalTexturePath.find("flat_nm") != std::string::npos, "detailNormalBlend 0 binds no detail normal");

    Require(named("Grille").alphaMode == MaterialAlphaMode::Mask, "a real cutout keeps its alpha test");
    Require(!named("Panel").detailLayers.IsEnabled(), "a flat detail stays a base-colour tint");
    Require(named("Panel").baseColor[1] < named("Panel").baseColor[0], "tinted by the paint");
}

// A car's materials at AC's light scale (docs/design/2026-10-06-ac-light-scale-design.md): a kn5
// beside a data.acd, so the import treats it as a car.
std::vector<std::uint8_t> BuildLitCarKn5()
{
    ByteWriter writer;
    writer.Raw("sc6969", 6);
    writer.U32(5);
    std::vector<std::array<std::uint8_t, 4>> weave;
    for (int texel = 0; texel < 16; ++texel)
    {
        weave.push_back(texel % 2 == 0 ? std::array<std::uint8_t, 4>{90, 90, 90, 255} : std::array<std::uint8_t, 4>{30, 30, 30, 255});
    }
    std::vector<std::array<std::uint8_t, 4>> halfAlpha;
    for (int texel = 0; texel < 16; ++texel)
    {
        halfAlpha.push_back({120, 120, 120, static_cast<std::uint8_t>(texel < 8 ? 0 : 255)});
    }
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>> textures{
        {"trim.dds", DdsFlat(4, {50, 50, 50, 255})},
        {"atlas.dds", DdsFlat(4, {200, 200, 200, 255})},
        {"screen.dds", DdsFlat(4, {20, 200, 20, 255})},
        {"cockpit.dds", DdsBgra(4, 4, halfAlpha)},
        {"cloth.dds", DdsBgra(4, 4, weave)},
        {"template.dds", DdsFlat(4, {255, 255, 255, 0})},
        {"colour.dds", DdsFlat(4, {100, 100, 100, 255})},
    };
    writer.U32(static_cast<std::uint32_t>(textures.size()));
    for (const auto& [name, data] : textures)
    {
        writer.U32(1);
        writer.String(name);
        writer.Blob(data);
    }
    const std::vector<FixtureMaterial> materials{
        {"Rubber", "ksPerPixel", false, false, {{"ksDiffuse", 0.1f}, {"ksAmbient", 0.1f}}, {{"txDiffuse", "trim.dds"}}},
        {"Bright", "ksPerPixel", false, false, {{"ksDiffuse", 0.8f}, {"ksAmbient", 0.8f}}, {{"txDiffuse", "trim.dds"}}},
        {"Chrome", "ksPerPixelReflection", false, false,
         {{"ksDiffuse", 0.01f}, {"ksAmbient", 0.1f}, {"ksSpecularEXP", 100.0f}, {"fresnelC", 0.05f}, {"fresnelEXP", 0.2f},
          {"fresnelMaxLevel", 0.7f}, {"isAdditive", 2.0f}},
         {{"txDiffuse", "atlas.dds"}}},
        {"Lens", "ksPerPixelReflection", false, false,
         {{"ksDiffuse", 0.4f}, {"ksAmbient", 0.3f}, {"ksSpecularEXP", 150.0f}, {"fresnelC", 0.07f}, {"fresnelEXP", 2.5f},
          {"fresnelMaxLevel", 0.4f}},
         {{"txDiffuse", "atlas.dds"}}},
        {"Screen", "ksPerPixel", false, false, {{"ksDiffuse", 0.0f}, {"ksAmbient", 0.0f}, {"ksEmissive", 0.0f}},
         {{"txDiffuse", "screen.dds"}}, {{"ksEmissive", {2.6f, 2.6f, 2.6f}}}},
        {"Cloth", "ksPerPixelMultiMap_NMDetail", false, false,
         {{"ksDiffuse", 0.3f}, {"ksAmbient", 0.3f}, {"useDetail", 1.0f}, {"detailUVMultiplier", 20.0f}},
         {{"txDiffuse", "cockpit.dds"}, {"txDetail", "cloth.dds"}}},
        {"Paint", "ksPerPixelMultiMap", false, false,
         {{"ksDiffuse", 0.5f}, {"ksAmbient", 0.5f}, {"useDetail", 1.0f}, {"detailUVMultiplier", 1.0f}},
         {{"txDiffuse", "template.dds"}, {"txDetail", "colour.dds"}}},
    };
    writer.U32(static_cast<std::uint32_t>(materials.size()));
    for (const FixtureMaterial& material : materials)
    {
        WriteMaterial(writer, material);
    }
    WriteDummy(writer, "ROOT", static_cast<std::uint32_t>(materials.size()), kIdentity);
    for (std::uint32_t index = 0; index < materials.size(); ++index)
    {
        WriteMesh(writer, "MESH_" + std::to_string(index), index, static_cast<float>(index));
    }
    return writer.Bytes();
}

void CarMaterialsTakeAcsLightScale()
{
    ScopedDirectory scope;
    const std::filesystem::path kn5 = scope.Path() / "lit_car" / "lit_car.kn5";
    WriteFile(kn5, BuildLitCarKn5());
    // Unreadable car data still imports; its presence is what marks the kn5 as a car.
    WriteFile(kn5.parent_path() / "data.acd", std::vector<std::uint8_t>(16, 0));
    const Kn5ImportReport report = Kn5Importer::ConvertToGltf(kn5, scope.Path() / "assets" / "lit_car");
    const LoadedModelData model = ModelLoader::LoadModel(report.gltfPath.string());
    const std::filesystem::path folder = report.gltfPath.parent_path();
    const auto named = [&model](const std::string& name) -> const ModelMaterialData&
    {
        for (const ModelMaterialData& material : model.materials)
        {
            if (material.name == name)
            {
                return material;
            }
        }
        throw std::runtime_error("no material " + name);
    };
    int width = 0;
    int height = 0;

    const ModelMaterialData& rubber = named("Rubber");
    RequireNear(rubber.baseColor[0], std::pow(0.4f, 2.2f), 1e-4f, "gain 0.4 is a factor of 0.4^2.2");
    Require(rubber.baseColorTexturePath.find("trim") != std::string::npos, "the diffuse itself stays the map");

    const ModelMaterialData& bright = named("Bright");
    RequireNear(bright.baseColor[0], 1.0f, 0.0f, "a gain above 1 is not a factor");
    Require(bright.baseColorTexturePath.find("_lit") != std::string::npos, "it is baked into the map");
    const std::vector<std::uint8_t> lit = ReadPngRgba((folder / bright.baseColorTexturePath).string(), width, height);
    Require(lit[0] == 160, "50 x gain 3.2 is 160, got " + std::to_string(lit[0]));

    const ModelMaterialData& chrome = named("Chrome");
    RequireNear(chrome.metallicFactor, 1.0f, 0.0f, "a reflector whose reflection outweighs its diffuse is metal");
    RequireNear(chrome.baseColor[0], std::pow(Kn5Importer::MeanReflection(0.05f, 0.2f, 0.7f, 2), 2.2f), 1e-3f,
                "grey at AC's mean reflection, made linear");
    Require(chrome.baseColorTexturePath.empty(), "AC's reflection is not tinted by the diffuse");
    RequireNear(chrome.roughnessFactor, 0.04f, 1e-6f, "isAdditive 2 at exponent 100 samples the sharp cube map");

    const ModelMaterialData& lens = named("Lens");
    RequireNear(lens.metallicFactor, 0.0f, 0.0f, "a lens keeps its diffuse: dielectric");

    const ModelMaterialData& screen = named("Screen");
    Require(screen.emissiveTexturePath.find("screen") != std::string::npos, "the diffuse is what glows");
    RequireNear(screen.emissiveColor[1], 1.0f, 0.0f, "a grey ksEmissive is white");
    RequireNear(screen.emissiveIntensity, 29900.0f, 100.0f, "ksEmissive 2.6, from valueC, is (2.6 x 2 / 4.84)^2.2 x 25 500");
    RequireNear(screen.baseColor[0], 0.0f, 0.0f, "no diffuse at ksDiffuse = ksAmbient = 0");

    const ModelMaterialData& cloth = named("Cloth");
    RequireNear(cloth.detailLayers.intensity, 1.2f, 1e-4f, "the tiled detail carries the gain");
    Require(!cloth.detailLayers.layerTexturePaths[1].empty(), "a half-alpha diffuse keeps a neutral layer");
    const std::vector<std::uint8_t> neutral =
        ReadPngRgba((folder / cloth.detailLayers.layerTexturePaths[1]).string(), width, height);
    Require(neutral[0] == 255, "white: where the diffuse is kept, only the gain applies");

    const ModelMaterialData& paint = named("Paint");
    RequireNear(paint.baseColor[0], 0.57758f, 1e-3f, "the colour 100/255 x gain 2, made linear");
}

int main()
{
    try
    {
        DdsDecodesBc1();
        DdsDecodesBc3AndBc4();
        DdsDecodesMaskedFormats();
        DdsRejectsBadInput();
        ReaderParsesTheContainer();
        RulesMatchTheConverter();
        ConversionReportsProgressToCompletion();
        ConversionIsDeterministicAcrossTextureThreads();
        ConvertsHierarchyGeometryAndMaterials();
        SkinChoiceChangesThePaint();
        RefusesEncryptedAndExistingTargets();
        ImportGoesThroughTheModelLoader();
        InspectOffersLiveriesAndOptions();
        PaintRankingPrefersTheBodywork();
        ReadsTrackLayouts();
        ImportsAWholeTrackLayout();
        ImportsMultilayerSurfacesAsDetailLayers();
        ImportsMultiMapDetailAsDetailLayers();
        CarMaterialsTakeAcsLightScale();
        AcdKeysMatchTheGame();
        AcdArchiveDecryptsAndRefusesAWrongFolder();
        CarDataBecomesASpec();
        ImportWritesTheCarsOwnData();
        FourWheelDriveRearSteerAndBodyBecomeASpec();
        ImportWritesFourWheelDriveRearSteerAndBody();
        LiveAxleDataBecomesASolidAxle();
        EveryInstalledCarDecrypts();

        std::cout << "kn5 import tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "kn5 import tests failed: " << error.what() << '\n';
        return 1;
    }
}
