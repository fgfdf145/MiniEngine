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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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
        for (int unused = 0; unused < 9; ++unused)
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
               std::uint32_t children = 0, bool renderable = true)
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
    writer.F32(0.0f); // lodIn
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
        mapTexels.push_back({static_cast<std::uint8_t>(texel * 17), 255, 0, 255});
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
        {"EXT_Carpaint", "ksPerPixelMultiMap_damage_dirt", false, false, {{"ksSpecular", 1.0f}, {"ksSpecularEXP", 50.0f}, {"useDetail", 1.0f}, {"fresnelMaxLevel", 0.6f}, {"sunSpecular", 12.0f}, {"sunSpecularEXP", 1500.0f}}, {{"txDiffuse", "Skin_00.dds"}, {"txDetail", "metal_detail.dds"}, {"txMaps", "car_MAP.dds"}, {"txNormal", "Car_Damage_NM.dds"}}},
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
    WriteFile(car / "skins" / "00_soul_red" / "METAL_DETAIL.dds", DdsFlat(4, {126, 1, 0, 255}));
    WriteFile(car / "skins" / "01_arctic_white" / "metal_detail.dds", DdsFlat(4, {213, 210, 208, 255}));
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
        RequireNear(model.materials[0].Property("sunSpecularEXP", 0.0f), 1500.0f, 0.0f, "material property");
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

    const std::set<std::string> twins =
        Kn5Importer::LowResTwins({"COCKPIT_HR", "cockpit_lr", "WHEEL_LR", "SUSP_LR", "STEER_HR", "STEER_LR"});
    Require(twins == std::set<std::string>{"cockpit_lr", "STEER_LR"}, "only _LR names with an _HR twin are low-res");

    RequireNear(Kn5Importer::SpecularExponentToRoughness(0.0f), 1.0f, 1e-6f, "exponent 0 is fully rough");
    RequireNear(Kn5Importer::SpecularExponentToRoughness(100.0f), 0.140028f, 1e-5f, "exponent 100");
    RequireNear(Kn5Importer::SpecularExponentToRoughness(1e6f), 0.04f, 1e-6f, "roughness floor");

    // Ceramic Metallic (148,148,148) doubles past white; Soul Red (126,1,0) does not.
    std::vector<std::uint8_t> grey(4 * 4, 148);
    const auto white = Kn5Importer::FlatDetailTint(grey, 2, 2);
    Require(white.has_value() && (*white)[0] == 1.0f, "a mid-grey detail doubles to white");
    std::vector<std::uint8_t> red{126, 1, 0, 255, 128, 2, 1, 255};
    const auto soulRed = Kn5Importer::FlatDetailTint(red, 2, 1);
    Require(soulRed.has_value(), "within 6 levels is one colour");
    RequireNear((*soulRed)[0], 0.99110f, 1e-4f, "red is doubled in gamma space, then linearised");
    std::vector<std::uint8_t> pattern{0, 0, 0, 255, 60, 0, 0, 255};
    Require(!Kn5Importer::FlatDetailTint(pattern, 2, 1).has_value(), "a pattern is not a paint colour");
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
    RequireNear(paint.specularFactor, 0.6f, 1e-4f, "fresnelMaxLevel is KHR_materials_specular");
    RequireNear(paint.clearcoatFactor, 0.6f, 1e-4f, "sunSpecular 12 is clearcoat 0.6");
    RequireNear(paint.clearcoatRoughnessFactor, 0.0365f, 1e-4f, "sunSpecularEXP 1500 is a tight clearcoat");
    Require(paint.normalTexturePath.empty(), "a damage dent map is not bound as a normal map");
    Require(paint.alphaMode == MaterialAlphaMode::Opaque, "paint is opaque");
    Require(std::filesystem::exists(folder / paint.baseColorTexturePath), "diffuse written");
    Require(PngChannels(folder / paint.baseColorTexturePath) == 3, "an opaque material's diffuse loses its alpha");

    const ModelMaterialData& leaves = model.materials[1];
    Require(leaves.alphaMode == MaterialAlphaMode::Mask, "alpha-tested is MASK");
    RequireNear(leaves.alphaCutoff, 0.5f, 0.0f, "ksAlphaRef 0 falls back to 0.5");
    RequireNear(leaves.roughnessFactor, 0.91287f, 1e-4f, "ksSpecular 0 is matte");
    Require(PngChannels(folder / leaves.baseColorTexturePath) == 4, "an alpha-tested diffuse keeps its alpha");

    const ModelMaterialData& glass = model.materials[2];
    Require(glass.alphaMode == MaterialAlphaMode::Blend, "alpha-blended is BLEND");
    Require(!glass.baseColorTexturePath.empty(), "a texture named in a different case still resolves");
    RequireNear(glass.roughnessFactor, 0.14003f, 1e-4f, "exponent 200 at intensity 0.5");
    Require(PngChannels(folder / glass.baseColorTexturePath) == 3, "a uniformly opaque alpha is dropped");

    const ModelMaterialData& lamp = model.materials[3];
    Require(lamp.baseColorTexturePath.empty(), "a stub texture is not written");
    RequireNear(lamp.emissiveColor[0], 1.0f, 0.0f, "ksEmissive clamps to 1");
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
    RequireNear(grey.materials[0].baseColor[0], 1.0f, 1e-6f, "the grey template doubles to white");

    Kn5ImportOptions white;
    white.skin = "01_ARCTIC_WHITE";
    const Kn5ImportReport chosen = Kn5Importer::ConvertToGltf(kn5, scope.Path() / "white", white);
    Require(chosen.skin == "01_arctic_white", "skins match ignoring case");

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
    WriteDummy(writer, meshName + "_ROOT", withCollisionMesh ? 2 : 1, kIdentity);
    WriteMesh(writer, meshName, 0, 1.0f);
    if (withCollisionMesh)
    {
        // A physics-only surface, as tracks ship them: the game collides with it but never draws it.
        WriteMesh(writer, meshName + "_PHYSICS", 0, 7.0f, false, 0, false);
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
    Require(summary.models == 2 && summary.meshes == 2 && summary.materials == 2, "a layout is surveyed across its models");
    Require(summary.hiddenMeshes == 1, "the collision-only mesh is counted apart from the drawn ones");
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
    const ModelSubmeshData& road = FindSubmesh(model, "1ROAD");
    const ModelSubmeshData& tree = FindSubmesh(model, "TREE");
    // Each mesh keeps the material of its own kn5, though both files index theirs from 0.
    Require(model.materials[road.materialIndex].name == "asphalt", "the main model's material");
    Require(model.materials[tree.materialIndex].name == "trees", "the second model's material, offset past the first's");
    // The trees' texture lives in the main kn5 only.
    Require(!model.materials[tree.materialIndex].baseColorTexturePath.empty(), "a texture another model of the layout carries resolves");

    // Unplaced: (1, 0, 0) under the axis root only. Placed: rotated to (0, 0, -1), moved by
    // (10, 0, 5) to (10, 0, 4), then the axis root's half turn.
    RequireNear(road.mesh.vertices[0].position[0], -1.0f, 1e-5f, "the unplaced model sits at the origin");
    RequireNear(tree.mesh.vertices[0].position[0], -10.0f, 1e-4f, "placed x");
    RequireNear(tree.mesh.vertices[0].position[2], -4.0f, 1e-4f, "placed z");
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
        ConvertsHierarchyGeometryAndMaterials();
        SkinChoiceChangesThePaint();
        RefusesEncryptedAndExistingTargets();
        ImportGoesThroughTheModelLoader();
        InspectOffersLiveriesAndOptions();
        PaintRankingPrefersTheBodywork();
        ReadsTrackLayouts();
        ImportsAWholeTrackLayout();

        std::cout << "kn5 import tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "kn5 import tests failed: " << error.what() << '\n';
        return 1;
    }
}
