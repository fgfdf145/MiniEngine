#include "kn5_reader.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace me
{

float Kn5Material::Property(const std::string& key, float fallback) const
{
    const auto found = properties.find(key);
    return found != properties.end() ? found->second : fallback;
}

std::string Kn5Material::Texture(const std::string& slot) const
{
    const auto found = textures.find(slot);
    return found != textures.end() ? found->second : std::string();
}

namespace
{
// Nodes nest one level per recursion; real models are a handful deep.
constexpr int kMaxNodeDepth = 512;
// type, name length, child count, active: the least a node can occupy.
constexpr size_t kMinNodeBytes = 13;

class ByteReader
{
  public:
    ByteReader(const std::vector<std::uint8_t>& bytes, const std::string& source)
        : m_bytes(bytes), m_source(source)
    {
    }

    size_t Offset() const
    {
        return m_offset;
    }

    size_t Remaining() const
    {
        return m_bytes.size() - m_offset;
    }

    void Require(size_t count) const
    {
        if (count > Remaining())
        {
            throw std::runtime_error(
                "'" + m_source + "' is truncated or not a kn5 (needed " + std::to_string(count) + " bytes at offset " +
                std::to_string(m_offset) + ")");
        }
    }

    void Skip(size_t count)
    {
        Require(count);
        m_offset += count;
    }

    std::uint8_t U8()
    {
        Require(1);
        return m_bytes[m_offset++];
    }

    std::uint32_t U32()
    {
        Require(4);
        std::uint32_t value = 0;
        std::memcpy(&value, m_bytes.data() + m_offset, 4);
        m_offset += 4;
        return value;
    }

    std::int32_t I32()
    {
        return static_cast<std::int32_t>(U32());
    }

    float F32()
    {
        Require(4);
        float value = 0.0f;
        std::memcpy(&value, m_bytes.data() + m_offset, 4);
        m_offset += 4;
        return value;
    }

    template <size_t N>
    void Floats(float* out)
    {
        Require(4 * N);
        std::memcpy(out, m_bytes.data() + m_offset, 4 * N);
        m_offset += 4 * N;
    }

    std::string String()
    {
        const std::uint32_t length = U32();
        Require(length);
        std::string value(reinterpret_cast<const char*>(m_bytes.data() + m_offset), length);
        m_offset += length;
        return value;
    }

    std::vector<std::uint8_t> Bytes(size_t count)
    {
        Require(count);
        std::vector<std::uint8_t> value(m_bytes.begin() + static_cast<std::ptrdiff_t>(m_offset),
                                        m_bytes.begin() + static_cast<std::ptrdiff_t>(m_offset + count));
        m_offset += count;
        return value;
    }

    // A count about to drive a loop whose every iteration reads at least `minBytesEach`.
    std::uint32_t Count(size_t minBytesEach, const char* what)
    {
        const std::uint32_t count = U32();
        if (minBytesEach > 0 && count > Remaining() / minBytesEach)
        {
            throw std::runtime_error(
                "'" + m_source + "' is corrupt: " + std::to_string(count) + " " + what + " at offset " +
                std::to_string(m_offset - 4) + " cannot fit in the file");
        }
        return count;
    }

    const std::string& Source() const
    {
        return m_source;
    }

  private:
    const std::vector<std::uint8_t>& m_bytes;
    const std::string& m_source;
    size_t m_offset = 0;
};

Kn5Node ReadNode(ByteReader& reader, bool readGeometry, int depth)
{
    if (depth > kMaxNodeDepth)
    {
        throw std::runtime_error("'" + reader.Source() + "' is corrupt: its node tree is nested too deeply");
    }

    Kn5Node node;
    const std::int32_t type = reader.I32();
    node.name = reader.String();
    const std::uint32_t childCount = reader.Count(kMinNodeBytes, "child nodes");
    node.active = reader.U8() != 0;

    if (type == static_cast<std::int32_t>(Kn5NodeType::Dummy))
    {
        node.type = Kn5NodeType::Dummy;
        reader.Floats<16>(node.matrix.data());
    }
    else if (type == static_cast<std::int32_t>(Kn5NodeType::Mesh) ||
             type == static_cast<std::int32_t>(Kn5NodeType::Skinned))
    {
        node.type = static_cast<Kn5NodeType>(type);
        const bool skinned = node.type == Kn5NodeType::Skinned;
        reader.Skip(3); // castShadows, isVisible, isTransparent
        if (skinned)
        {
            const std::uint32_t boneCount = reader.Count(4 + 64, "bones");
            for (std::uint32_t bone = 0; bone < boneCount; ++bone)
            {
                reader.String();
                reader.Skip(16 * 4); // bind pose
            }
        }

        // position(3) normal(3) uv(2) tangent(3), then weights(4) and bone indices(4) when skinned.
        const size_t stride = (11 + (skinned ? 8 : 0)) * 4;
        node.vertexCount = reader.Count(stride, "vertices");
        if (readGeometry)
        {
            node.vertices.resize(node.vertexCount);
            for (Kn5Vertex& vertex : node.vertices)
            {
                reader.Floats<3>(vertex.position.data());
                reader.Floats<3>(vertex.normal.data());
                reader.Floats<2>(vertex.uv.data());
                reader.Floats<3>(vertex.tangent.data());
                if (skinned)
                {
                    reader.Skip(8 * 4);
                }
            }
        }
        else
        {
            reader.Skip(node.vertexCount * stride);
        }

        const std::uint32_t indexCount = reader.Count(2, "indices");
        node.triangleCount = indexCount / 3;
        if (readGeometry)
        {
            const std::vector<std::uint8_t> raw = reader.Bytes(static_cast<size_t>(indexCount) * 2);
            node.indices.resize(indexCount);
            if (!raw.empty())
            {
                std::memcpy(node.indices.data(), raw.data(), raw.size());
            }
        }
        else
        {
            reader.Skip(static_cast<size_t>(indexCount) * 2);
        }

        node.materialIndex = reader.U32();
        reader.Skip(4);     // layer
        reader.Skip(4 * 2); // lodIn, lodOut
        // Only a plain mesh carries a bounding sphere and the isRenderable byte. A skinned one
        // ends at lodOut; reading them anyway desyncs the rest of the tree.
        if (node.type == Kn5NodeType::Mesh)
        {
            reader.Skip(4 * 4); // bounding sphere centre and radius
            reader.Skip(1);     // isRenderable
        }
    }
    else
    {
        throw std::runtime_error(
            "'" + reader.Source() + "' is corrupt: unknown node type " + std::to_string(type) + " at offset " +
            std::to_string(reader.Offset()));
    }

    node.children.reserve(childCount);
    for (std::uint32_t child = 0; child < childCount; ++child)
    {
        node.children.push_back(ReadNode(reader, readGeometry, depth + 1));
    }
    return node;
}
}

namespace Kn5Reader
{
Kn5Model Parse(const std::vector<std::uint8_t>& bytes, const std::string& source, bool readGeometry)
{
    Kn5Model model;
    const std::string_view marker(kEncryptionMarker);
    model.encrypted =
        std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end()) != bytes.end();

    ByteReader reader(bytes, source);
    reader.Require(6);
    if (std::memcmp(bytes.data(), "sc6969", 6) != 0)
    {
        throw std::runtime_error("'" + source + "' is not a kn5 file");
    }
    reader.Skip(6);
    model.version = reader.U32();
    if (model.version > 5)
    {
        reader.Skip(4); // one extra header word from v6 on
    }

    const std::uint32_t textureCount = reader.Count(12, "textures");
    model.textures.reserve(textureCount);
    for (std::uint32_t index = 0; index < textureCount; ++index)
    {
        Kn5Texture texture;
        texture.active = reader.U32() != 0;
        texture.name = reader.String();
        const std::uint32_t size = reader.U32();
        texture.data = reader.Bytes(size);
        model.textures.push_back(std::move(texture));
    }

    const std::uint32_t materialCount = reader.Count(18, "materials");
    model.materials.reserve(materialCount);
    for (std::uint32_t index = 0; index < materialCount; ++index)
    {
        Kn5Material material;
        material.name = reader.String();
        material.shader = reader.String();
        material.alphaBlend = reader.U8() != 0;
        material.alphaTested = reader.U8() != 0;
        reader.Skip(4); // depthMode: an int32, not a byte
        const std::uint32_t propertyCount = reader.Count(4 + 4 + 36, "material properties");
        for (std::uint32_t property = 0; property < propertyCount; ++property)
        {
            std::string name = reader.String();
            const float value = reader.F32();
            reader.Skip(4 * 9); // valueB(2) valueC(3) valueD(4), unused by any shader read here
            material.properties[std::move(name)] = value;
        }
        const std::uint32_t slotCount = reader.Count(12, "texture slots");
        for (std::uint32_t slot = 0; slot < slotCount; ++slot)
        {
            std::string slotName = reader.String();
            reader.Skip(4); // shader register
            material.textures[std::move(slotName)] = reader.String();
        }
        model.materials.push_back(std::move(material));
    }

    model.root = ReadNode(reader, readGeometry, 0);
    return model;
}

Kn5Model Load(const std::filesystem::path& path, bool readGeometry)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("Cannot open '" + path.string() + "'");
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad())
    {
        throw std::runtime_error("Cannot read '" + path.string() + "'");
    }
    return Parse(bytes, path.string(), readGeometry);
}
}
}
