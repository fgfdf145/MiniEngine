#include "kn5_reader.h"

#include <algorithm>
#include <cstring>
#include <fstream>
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

// Reads a kn5 from a stream, checking every read against the stream's size, so a corrupt count
// fails with a message instead of an allocation the size of the count.
class ByteReader
{
  public:
    ByteReader(std::istream& stream, size_t size, const std::string& source)
        : m_stream(stream), m_size(size), m_source(source)
    {
    }

    size_t Offset() const
    {
        return m_offset;
    }

    size_t Remaining() const
    {
        return m_size - m_offset;
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
        m_stream.seekg(static_cast<std::streamoff>(count), std::ios::cur);
        m_offset += count;
    }

    void Read(void* out, size_t count)
    {
        Require(count);
        if (count > 0 && !m_stream.read(static_cast<char*>(out), static_cast<std::streamsize>(count)))
        {
            throw std::runtime_error("Cannot read '" + m_source + "' at offset " + std::to_string(m_offset));
        }
        m_offset += count;
    }

    std::uint8_t U8()
    {
        std::uint8_t value = 0;
        Read(&value, 1);
        return value;
    }

    std::uint32_t U32()
    {
        std::uint32_t value = 0;
        Read(&value, 4);
        return value;
    }

    std::int32_t I32()
    {
        return static_cast<std::int32_t>(U32());
    }

    float F32()
    {
        float value = 0.0f;
        Read(&value, 4);
        return value;
    }

    std::string String()
    {
        const std::uint32_t length = U32();
        Require(length);
        std::string value(length, '\0');
        Read(value.data(), length);
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
    std::istream& m_stream;
    size_t m_size = 0;
    const std::string& m_source;
    size_t m_offset = 0;
};

// A stream over bytes already in memory, without copying them.
class MemoryBuffer : public std::streambuf
{
  public:
    explicit MemoryBuffer(const std::vector<std::uint8_t>& bytes)
    {
        char* begin = const_cast<char*>(reinterpret_cast<const char*>(bytes.data()));
        setg(begin, begin, begin + bytes.size());
    }

  protected:
    pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode) override
    {
        char* target = direction == std::ios_base::beg   ? eback() + offset
                       : direction == std::ios_base::cur ? gptr() + offset
                                                         : egptr() + offset;
        if (target < eback() || target > egptr())
        {
            return pos_type(off_type(-1));
        }
        setg(eback(), target, egptr());
        return pos_type(target - eback());
    }

    pos_type seekpos(pos_type position, std::ios_base::openmode mode) override
    {
        return seekoff(off_type(position), std::ios_base::beg, mode);
    }
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
        reader.Read(node.matrix.data(), 16 * 4);
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
                reader.Read(vertex.position.data(), 3 * 4);
                reader.Read(vertex.normal.data(), 3 * 4);
                reader.Read(vertex.uv.data(), 2 * 4);
                reader.Read(vertex.tangent.data(), 3 * 4);
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
            node.indices.resize(indexCount);
            reader.Read(node.indices.data(), static_cast<size_t>(indexCount) * 2);
        }
        else
        {
            reader.Skip(static_cast<size_t>(indexCount) * 2);
        }

        node.materialIndex = reader.U32();
        reader.Skip(4);     // layer
        node.lodIn = reader.F32();
        reader.Skip(4); // lodOut
        // Only a plain mesh carries a bounding sphere and the isRenderable byte. A skinned one
        // ends at lodOut; reading them anyway desyncs the rest of the tree.
        if (node.type == Kn5NodeType::Mesh)
        {
            reader.Skip(4 * 4); // bounding sphere centre and radius
            node.renderable = reader.U8() != 0;
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

Kn5Model ReadModel(std::istream& stream, size_t size, const std::string& source, Kn5ReadScope scope)
{
    ByteReader reader(stream, size, source);
    char magic[6] = {};
    reader.Read(magic, sizeof(magic));
    if (std::memcmp(magic, "sc6969", sizeof(magic)) != 0)
    {
        throw std::runtime_error("'" + source + "' is not a kn5 file");
    }
    Kn5Model model;
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
        texture.size = reader.U32();
        if (scope == Kn5ReadScope::Tables)
        {
            reader.Skip(texture.size);
        }
        else
        {
            reader.Require(texture.size);
            texture.data.resize(texture.size);
            reader.Read(texture.data.data(), texture.size);
        }
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

    if (scope == Kn5ReadScope::NoGeometry || scope == Kn5ReadScope::Everything)
    {
        model.root = ReadNode(reader, scope == Kn5ReadScope::Everything, 0);
    }
    return model;
}
}

namespace Kn5Reader
{
Kn5Model Parse(const std::vector<std::uint8_t>& bytes, const std::string& source, Kn5ReadScope scope)
{
    MemoryBuffer buffer(bytes);
    std::istream stream(&buffer);
    return ReadModel(stream, bytes.size(), source, scope);
}

Kn5Model Load(const std::filesystem::path& path, Kn5ReadScope scope)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        throw std::runtime_error("Cannot open '" + path.string() + "'");
    }
    const std::streamoff size = file.tellg();
    file.seekg(0);
    return ReadModel(file, static_cast<size_t>(std::max<std::streamoff>(size, 0)), path.string(), scope);
}

bool IsEncrypted(const std::vector<std::uint8_t>& bytes)
{
    const std::string_view marker(kEncryptionMarker);
    return std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end()) != bytes.end();
}

bool IsEncrypted(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("Cannot open '" + path.string() + "'");
    }
    // In chunks, each overlapping the last by the marker's length less one, so a marker across
    // a chunk boundary is still found.
    const std::string_view marker(kEncryptionMarker);
    std::vector<char> chunk(1 << 20);
    size_t carried = 0;
    while (file)
    {
        file.read(chunk.data() + carried, static_cast<std::streamsize>(chunk.size() - carried));
        const size_t filled = carried + static_cast<size_t>(file.gcount());
        if (std::search(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(filled), marker.begin(), marker.end()) !=
            chunk.begin() + static_cast<std::ptrdiff_t>(filled))
        {
            return true;
        }
        carried = std::min(filled, marker.size() - 1);
        std::memmove(chunk.data(), chunk.data() + filled - carried, carried);
    }
    return false;
}
}
}
