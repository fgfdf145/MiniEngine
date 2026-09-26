#include "gltf_compression.h"

#include <tiny_gltf.h>

#include <draco/compression/decode.h>
#include <meshoptimizer.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace me
{

namespace
{
constexpr std::array<std::string_view, 2> kMeshoptExtensions = {"EXT_meshopt_compression", "KHR_meshopt_compression"};

// The meshopt extension object on a buffer or buffer view, whichever of the two names it uses.
const tinygltf::Value* FindMeshoptExtension(const tinygltf::ExtensionMap& extensions)
{
    for (std::string_view name : kMeshoptExtensions)
    {
        if (const auto found = extensions.find(std::string(name)); found != extensions.end() && found->second.IsObject())
        {
            return &found->second;
        }
    }
    return nullptr;
}

size_t ReadSize(const tinygltf::Value& object, const char* key, size_t fallback)
{
    if (!object.Has(key))
    {
        return fallback;
    }
    const tinygltf::Value& value = object.Get(key);
    if (!value.IsNumber() && !value.IsInt())
    {
        throw std::runtime_error(std::string("glTF meshopt extension has a non-numeric ") + key);
    }
    const double number = value.GetNumberAsDouble();
    if (number < 0.0 || number > static_cast<double>(std::numeric_limits<uint32_t>::max()))
    {
        throw std::runtime_error(std::string("glTF meshopt extension has an out of range ") + key);
    }
    return static_cast<size_t>(number);
}

std::string ReadString(const tinygltf::Value& object, const char* key, const char* fallback)
{
    return object.Has(key) && object.Get(key).IsString() ? object.Get(key).Get<std::string>() : std::string(fallback);
}

void DecodeMeshoptBufferView(tinygltf::Model& model, tinygltf::BufferView& view, const tinygltf::Value& extension)
{
    const size_t sourceIndex = ReadSize(extension, "buffer", std::numeric_limits<size_t>::max());
    if (sourceIndex >= model.buffers.size())
    {
        throw std::runtime_error("glTF meshopt buffer view names a buffer that does not exist");
    }
    const size_t byteOffset = ReadSize(extension, "byteOffset", 0);
    const size_t byteLength = ReadSize(extension, "byteLength", std::numeric_limits<size_t>::max());
    const size_t byteStride = ReadSize(extension, "byteStride", 0);
    const size_t count = ReadSize(extension, "count", std::numeric_limits<size_t>::max());
    const std::string mode = ReadString(extension, "mode", "");
    const std::string filter = ReadString(extension, "filter", "NONE");

    const std::vector<unsigned char>& source = model.buffers[sourceIndex].data;
    if (byteLength == std::numeric_limits<size_t>::max() || count == std::numeric_limits<size_t>::max() ||
        byteOffset > source.size() || byteLength > source.size() - byteOffset)
    {
        throw std::runtime_error("glTF meshopt buffer view points outside its buffer");
    }
    if (byteStride == 0 || byteStride > 256)
    {
        throw std::runtime_error("glTF meshopt buffer view has an invalid byteStride");
    }

    std::vector<unsigned char> decoded(count * byteStride);
    const unsigned char* compressed = source.data() + byteOffset;
    int result = -1;
    if (mode == "ATTRIBUTES")
    {
        result = meshopt_decodeVertexBuffer(decoded.data(), count, byteStride, compressed, byteLength);
    }
    else if (mode == "TRIANGLES" || mode == "INDICES")
    {
        if (byteStride != 2 && byteStride != 4)
        {
            throw std::runtime_error("glTF meshopt index buffer view has a byteStride other than 2 or 4");
        }
        result = mode == "TRIANGLES"
                     ? meshopt_decodeIndexBuffer(decoded.data(), count, byteStride, compressed, byteLength)
                     : meshopt_decodeIndexSequence(decoded.data(), count, byteStride, compressed, byteLength);
    }
    else
    {
        throw std::runtime_error("glTF meshopt buffer view has unknown mode '" + mode + "'");
    }
    if (result != 0)
    {
        throw std::runtime_error("glTF meshopt buffer view failed to decode (" + mode + ", error " + std::to_string(result) + ")");
    }

    if (filter == "OCTAHEDRAL")
    {
        meshopt_decodeFilterOct(decoded.data(), count, byteStride);
    }
    else if (filter == "QUATERNION")
    {
        meshopt_decodeFilterQuat(decoded.data(), count, byteStride);
    }
    else if (filter == "EXPONENTIAL")
    {
        meshopt_decodeFilterExp(decoded.data(), count, byteStride);
    }
    else if (filter == "COLOR")
    {
        meshopt_decodeFilterColor(decoded.data(), count, byteStride);
    }
    else if (filter != "NONE")
    {
        throw std::runtime_error("glTF meshopt buffer view has unknown filter '" + filter + "'");
    }

    tinygltf::Buffer buffer{};
    buffer.name = view.name.empty() ? "meshopt decoded" : view.name + " (meshopt decoded)";
    buffer.data = std::move(decoded);
    view.buffer = static_cast<int>(model.buffers.size());
    view.byteOffset = 0;
    view.byteLength = buffer.data.size();
    model.buffers.push_back(std::move(buffer));
}

// Appends count elements of numComponents values of T, each read from the draco attribute at a
// point, to data. Returns false when draco cannot convert the attribute's values to T.
template <typename T>
bool AppendDracoValues(const draco::PointAttribute& attribute, uint32_t pointCount, int numComponents, std::vector<unsigned char>& data)
{
    std::array<T, 16> values{};
    for (uint32_t point = 0; point < pointCount; ++point)
    {
        if (!attribute.ConvertValue<T>(attribute.mapped_index(draco::PointIndex(point)), static_cast<int8_t>(numComponents), values.data()))
        {
            return false;
        }
        const size_t offset = data.size();
        data.resize(offset + sizeof(T) * static_cast<size_t>(numComponents));
        std::memcpy(data.data() + offset, values.data(), sizeof(T) * static_cast<size_t>(numComponents));
    }
    return true;
}

bool AppendDracoAttribute(const draco::PointAttribute& attribute, uint32_t pointCount, int componentType, int numComponents, std::vector<unsigned char>& data)
{
    switch (componentType)
    {
    case TINYGLTF_COMPONENT_TYPE_FLOAT:
        return AppendDracoValues<float>(attribute, pointCount, numComponents, data);
    case TINYGLTF_COMPONENT_TYPE_BYTE:
        return AppendDracoValues<int8_t>(attribute, pointCount, numComponents, data);
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        return AppendDracoValues<uint8_t>(attribute, pointCount, numComponents, data);
    case TINYGLTF_COMPONENT_TYPE_SHORT:
        return AppendDracoValues<int16_t>(attribute, pointCount, numComponents, data);
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        return AppendDracoValues<uint16_t>(attribute, pointCount, numComponents, data);
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        return AppendDracoValues<uint32_t>(attribute, pointCount, numComponents, data);
    default:
        throw std::runtime_error("glTF Draco attribute accessor has an unsupported component type");
    }
}

// Starts the next buffer view on a four-byte boundary, as accessors of any component type need.
void AlignTo4(std::vector<unsigned char>& data)
{
    data.resize((data.size() + 3) & ~static_cast<size_t>(3), 0);
}

void PointAccessorAt(tinygltf::Model& model, tinygltf::Accessor& accessor, int bufferIndex, size_t byteOffset, size_t byteLength)
{
    tinygltf::BufferView view{};
    view.buffer = bufferIndex;
    view.byteOffset = byteOffset;
    view.byteLength = byteLength;
    accessor.bufferView = static_cast<int>(model.bufferViews.size());
    accessor.byteOffset = 0;
    accessor.sparse = tinygltf::Accessor::Sparse{};
    model.bufferViews.push_back(view);
}

void DecodeDracoPrimitive(tinygltf::Model& model, tinygltf::Primitive& primitive, const tinygltf::Value& extension)
{
    const int viewIndex = extension.Has("bufferView") ? extension.Get("bufferView").GetNumberAsInt() : -1;
    if (viewIndex < 0 || static_cast<size_t>(viewIndex) >= model.bufferViews.size())
    {
        throw std::runtime_error("glTF Draco primitive names a buffer view that does not exist");
    }
    const tinygltf::BufferView& view = model.bufferViews[static_cast<size_t>(viewIndex)];
    if (view.buffer < 0 || static_cast<size_t>(view.buffer) >= model.buffers.size())
    {
        throw std::runtime_error("glTF Draco buffer view names a buffer that does not exist");
    }
    const std::vector<unsigned char>& source = model.buffers[static_cast<size_t>(view.buffer)].data;
    if (view.byteOffset > source.size() || view.byteLength > source.size() - view.byteOffset)
    {
        throw std::runtime_error("glTF Draco buffer view points outside its buffer");
    }

    draco::DecoderBuffer compressed;
    compressed.Init(reinterpret_cast<const char*>(source.data() + view.byteOffset), view.byteLength);
    draco::Decoder decoder;
    draco::StatusOr<std::unique_ptr<draco::Mesh>> decodedMesh = decoder.DecodeMeshFromBuffer(&compressed);
    if (!decodedMesh.ok())
    {
        throw std::runtime_error("glTF Draco primitive failed to decode: " + decodedMesh.status().error_msg_string());
    }
    const std::unique_ptr<draco::Mesh> mesh = std::move(decodedMesh).value();
    const uint32_t pointCount = mesh->num_points();

    std::vector<unsigned char> data;
    std::vector<std::pair<int, std::pair<size_t, size_t>>> placements; // accessor, (offset, length)

    const tinygltf::Value& attributes = extension.Get("attributes");
    if (!attributes.IsObject())
    {
        throw std::runtime_error("glTF Draco primitive has no attributes object");
    }
    for (const std::string& semantic : attributes.Keys())
    {
        const auto accessorIt = primitive.attributes.find(semantic);
        if (accessorIt == primitive.attributes.end())
        {
            continue; // compressed, but not an attribute this primitive declares
        }
        const int accessorIndex = accessorIt->second;
        if (accessorIndex < 0 || static_cast<size_t>(accessorIndex) >= model.accessors.size())
        {
            throw std::runtime_error("glTF Draco primitive's " + semantic + " accessor does not exist");
        }
        const draco::PointAttribute* attribute =
            mesh->GetAttributeByUniqueId(static_cast<uint32_t>(attributes.Get(semantic).GetNumberAsInt()));
        if (attribute == nullptr)
        {
            throw std::runtime_error("glTF Draco primitive's " + semantic + " attribute is missing from the compressed data");
        }

        const tinygltf::Accessor& accessor = model.accessors[static_cast<size_t>(accessorIndex)];
        const int numComponents = tinygltf::GetNumComponentsInType(static_cast<uint32_t>(accessor.type));
        if (numComponents <= 0 || numComponents > 16)
        {
            throw std::runtime_error("glTF Draco primitive's " + semantic + " accessor has an invalid type");
        }
        AlignTo4(data);
        const size_t offset = data.size();
        if (!AppendDracoAttribute(*attribute, pointCount, accessor.componentType, numComponents, data))
        {
            throw std::runtime_error("glTF Draco primitive's " + semantic + " attribute cannot be read as its accessor's type");
        }
        placements.push_back({accessorIndex, {offset, data.size() - offset}});
    }

    std::optional<std::pair<size_t, size_t>> indexPlacement;
    if (mesh->num_faces() > 0)
    {
        AlignTo4(data);
        const size_t offset = data.size();
        data.resize(offset + static_cast<size_t>(mesh->num_faces()) * 3 * sizeof(uint32_t));
        auto* indices = reinterpret_cast<uint32_t*>(data.data() + offset);
        for (uint32_t face = 0; face < mesh->num_faces(); ++face)
        {
            const draco::Mesh::Face& corners = mesh->face(draco::FaceIndex(face));
            for (size_t corner = 0; corner < 3; ++corner)
            {
                indices[face * 3 + corner] = corners[corner].value();
            }
        }
        indexPlacement = std::pair<size_t, size_t>{offset, data.size() - offset};
    }

    tinygltf::Buffer buffer{};
    buffer.name = "draco decoded";
    buffer.data = std::move(data);
    const int bufferIndex = static_cast<int>(model.buffers.size());
    model.buffers.push_back(std::move(buffer));

    for (const auto& [accessorIndex, placement] : placements)
    {
        tinygltf::Accessor& accessor = model.accessors[static_cast<size_t>(accessorIndex)];
        PointAccessorAt(model, accessor, bufferIndex, placement.first, placement.second);
        accessor.count = pointCount;
    }
    if (indexPlacement)
    {
        if (primitive.indices < 0)
        {
            tinygltf::Accessor indices{};
            indices.type = TINYGLTF_TYPE_SCALAR;
            primitive.indices = static_cast<int>(model.accessors.size());
            model.accessors.push_back(indices);
        }
        else if (static_cast<size_t>(primitive.indices) >= model.accessors.size())
        {
            throw std::runtime_error("glTF Draco primitive's index accessor does not exist");
        }
        tinygltf::Accessor& indices = model.accessors[static_cast<size_t>(primitive.indices)];
        PointAccessorAt(model, indices, bufferIndex, indexPlacement->first, indexPlacement->second);
        indices.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
        indices.normalized = false;
        indices.count = static_cast<size_t>(mesh->num_faces()) * 3;
    }
}
}

std::optional<std::string> ReplaceMeshoptFallbackBuffers(const std::string& json)
{
    if (json.find("meshopt_compression") == std::string::npos)
    {
        return std::nullopt;
    }
    nlohmann::ordered_json document = nlohmann::ordered_json::parse(json);
    const auto buffers = document.find("buffers");
    if (buffers == document.end() || !buffers->is_array())
    {
        return std::nullopt;
    }

    bool replaced = false;
    for (nlohmann::ordered_json& buffer : *buffers)
    {
        if (buffer.contains("uri") || !buffer.contains("extensions"))
        {
            continue;
        }
        const nlohmann::ordered_json& extensions = buffer["extensions"];
        for (std::string_view name : kMeshoptExtensions)
        {
            const auto extension = extensions.find(std::string(name));
            if (extension != extensions.end() && extension->is_object() && extension->value("fallback", false))
            {
                buffer["uri"] = "data:application/octet-stream;base64,AA==";
                buffer["byteLength"] = 1;
                replaced = true;
                break;
            }
        }
    }
    return replaced ? std::optional<std::string>(document.dump()) : std::nullopt;
}

void DecodeMeshoptBufferViews(tinygltf::Model& model)
{
    // Indexing, not iterating: decoding appends to model.buffers, never to the views.
    for (size_t index = 0; index < model.bufferViews.size(); ++index)
    {
        tinygltf::BufferView& view = model.bufferViews[index];
        if (const tinygltf::Value* extension = FindMeshoptExtension(view.extensions))
        {
            DecodeMeshoptBufferView(model, view, *extension);
        }
    }
}

void DecodeDracoPrimitives(tinygltf::Model& model)
{
    for (tinygltf::Mesh& mesh : model.meshes)
    {
        for (tinygltf::Primitive& primitive : mesh.primitives)
        {
            const auto extension = primitive.extensions.find("KHR_draco_mesh_compression");
            if (extension != primitive.extensions.end() && extension->second.IsObject())
            {
                DecodeDracoPrimitive(model, primitive, extension->second);
            }
        }
    }
}
}
