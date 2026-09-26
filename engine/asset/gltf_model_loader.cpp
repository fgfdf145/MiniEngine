#include "gltf_model_loader.h"

#include "model_post_process.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_INCLUDE_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <stb_image.h>
#include <tiny_gltf.h>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <engine/core/log/log.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace me
{

namespace
{
constexpr int kGltfModePoints = 0;
constexpr int kGltfModeLines = 1;
constexpr int kGltfModeLineLoop = 2;
constexpr int kGltfModeLineStrip = 3;
constexpr int kGltfModeTriangles = 4;
constexpr int kGltfModeTriangleStrip = 5;
constexpr int kGltfModeTriangleFan = 6;

std::string ToLowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
                   {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

std::string SanitizeFileName(std::string value)
{
    if (value.empty())
    {
        return "asset";
    }

    for (char& character : value)
    {
        const bool keepCharacter =
            std::isalnum(static_cast<unsigned char>(character)) ||
            character == '_' ||
            character == '-' ||
            character == '.';
        if (!keepCharacter)
        {
            character = '_';
        }
    }

    return value;
}

bool IsHexDigit(char character)
{
    return std::isxdigit(static_cast<unsigned char>(character)) != 0;
}

int HexDigitToInt(char character)
{
    if (character >= '0' && character <= '9')
    {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f')
    {
        return 10 + (character - 'a');
    }
    if (character >= 'A' && character <= 'F')
    {
        return 10 + (character - 'A');
    }
    return 0;
}

std::string DecodeUriPath(std::string_view uri)
{
    std::string decoded;
    decoded.reserve(uri.size());

    for (size_t index = 0; index < uri.size(); ++index)
    {
        const char character = uri[index];
        if (character == '%' && index + 2 < uri.size() && IsHexDigit(uri[index + 1]) && IsHexDigit(uri[index + 2]))
        {
            const int value = (HexDigitToInt(uri[index + 1]) << 4) | HexDigitToInt(uri[index + 2]);
            decoded.push_back(static_cast<char>(value));
            index += 2;
            continue;
        }

        decoded.push_back(character);
    }

    return decoded;
}

// Inverse of DecodeUriPath for URIs we write back into a .gltf: keeps RFC 3986
// unreserved characters and '/' as-is, percent-encodes everything else.
std::string EncodeUriPath(std::string_view path)
{
    constexpr char kHexDigits[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(path.size());

    for (const char character : path)
    {
        const auto value = static_cast<unsigned char>(character);
        const bool unreserved =
            std::isalnum(value) != 0 ||
            character == '-' || character == '_' ||
            character == '.' || character == '~' ||
            character == '/';
        if (unreserved)
        {
            encoded.push_back(character);
        }
        else
        {
            encoded.push_back('%');
            encoded.push_back(kHexDigits[value >> 4]);
            encoded.push_back(kHexDigits[value & 0x0F]);
        }
    }

    return encoded;
}

std::string BuildEmbeddedTextureFileName(const tinygltf::Image& image, size_t imageIndex)
{
    const std::string baseName =
        SanitizeFileName(image.name.empty() ? ("image_" + std::to_string(imageIndex)) : image.name);
    return baseName + "_" + std::to_string(imageIndex) + ".png";
}

void EnsureIndexInRange(size_t index, size_t size, const char* label)
{
    if (index >= size)
    {
        throw std::runtime_error(std::string("glTF references an invalid ") + label + " index");
    }
}

double ReadComponentAsDouble(const unsigned char* data, int componentType, bool normalized)
{
    switch (componentType)
    {
    case TINYGLTF_COMPONENT_TYPE_BYTE:
    {
        const int8_t value = *reinterpret_cast<const int8_t*>(data);
        if (!normalized)
        {
            return static_cast<double>(value);
        }
        return std::max(static_cast<double>(value) / 127.0, -1.0);
    }
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
    {
        const uint8_t value = *reinterpret_cast<const uint8_t*>(data);
        return normalized ? static_cast<double>(value) / 255.0 : static_cast<double>(value);
    }
    case TINYGLTF_COMPONENT_TYPE_SHORT:
    {
        const int16_t value = *reinterpret_cast<const int16_t*>(data);
        if (!normalized)
        {
            return static_cast<double>(value);
        }
        return std::max(static_cast<double>(value) / 32767.0, -1.0);
    }
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
    {
        const uint16_t value = *reinterpret_cast<const uint16_t*>(data);
        return normalized ? static_cast<double>(value) / 65535.0 : static_cast<double>(value);
    }
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
    {
        const uint32_t value = *reinterpret_cast<const uint32_t*>(data);
        return normalized ? static_cast<double>(value) / 4294967295.0 : static_cast<double>(value);
    }
    case TINYGLTF_COMPONENT_TYPE_FLOAT:
        return static_cast<double>(*reinterpret_cast<const float*>(data));
    case TINYGLTF_COMPONENT_TYPE_DOUBLE:
        return *reinterpret_cast<const double*>(data);
    default:
        throw std::runtime_error("glTF accessor uses an unsupported component type");
    }
}

uint32_t ReadIndexComponent(const unsigned char* data, int componentType)
{
    switch (componentType)
    {
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        return static_cast<uint32_t>(*reinterpret_cast<const uint8_t*>(data));
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        return static_cast<uint32_t>(*reinterpret_cast<const uint16_t*>(data));
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        return *reinterpret_cast<const uint32_t*>(data);
    default:
        throw std::runtime_error("glTF indices must use unsigned integer component types");
    }
}

const unsigned char* GetAccessorDataPointer(
    const tinygltf::Model& model,
    const tinygltf::Accessor& accessor,
    const tinygltf::BufferView& bufferView,
    size_t elementIndex,
    size_t elementByteSize)
{
    EnsureIndexInRange(static_cast<size_t>(bufferView.buffer), model.buffers.size(), "buffer");
    const tinygltf::Buffer& buffer = model.buffers[static_cast<size_t>(bufferView.buffer)];
    const int stride = accessor.ByteStride(bufferView);
    if (stride <= 0)
    {
        throw std::runtime_error("glTF accessor has an invalid byte stride");
    }

    const size_t elementOffset = bufferView.byteOffset + accessor.byteOffset + elementIndex * static_cast<size_t>(stride);
    const size_t bufferViewEnd = bufferView.byteOffset + bufferView.byteLength;
    if (elementOffset + elementByteSize > buffer.data.size() || elementOffset + elementByteSize > bufferViewEnd)
    {
        throw std::runtime_error("glTF accessor points outside the underlying buffer");
    }

    return buffer.data.data() + elementOffset;
}

const unsigned char* GetBufferViewDataPointer(
    const tinygltf::Model& model,
    const tinygltf::BufferView& bufferView,
    size_t byteOffset,
    size_t byteSize)
{
    EnsureIndexInRange(static_cast<size_t>(bufferView.buffer), model.buffers.size(), "buffer");
    const tinygltf::Buffer& buffer = model.buffers[static_cast<size_t>(bufferView.buffer)];
    const size_t absoluteOffset = bufferView.byteOffset + byteOffset;
    const size_t bufferViewEnd = bufferView.byteOffset + bufferView.byteLength;
    if (absoluteOffset + byteSize > buffer.data.size() || absoluteOffset + byteSize > bufferViewEnd)
    {
        throw std::runtime_error("glTF sparse accessor points outside the underlying buffer");
    }

    return buffer.data.data() + absoluteOffset;
}

std::vector<uint32_t> ReadSparseAccessorIndices(
    const tinygltf::Model& model,
    const tinygltf::Accessor& accessor)
{
    if (!accessor.sparse.isSparse)
    {
        return {};
    }

    EnsureIndexInRange(static_cast<size_t>(accessor.sparse.indices.bufferView), model.bufferViews.size(), "buffer view");
    const tinygltf::BufferView& indicesBufferView =
        model.bufferViews[static_cast<size_t>(accessor.sparse.indices.bufferView)];
    const int componentSize = tinygltf::GetComponentSizeInBytes(
        static_cast<uint32_t>(accessor.sparse.indices.componentType));
    if (componentSize <= 0)
    {
        throw std::runtime_error("glTF sparse accessor index component has an invalid size");
    }

    if (accessor.sparse.count < 0 || static_cast<size_t>(accessor.sparse.count) > accessor.count)
    {
        throw std::runtime_error("glTF sparse accessor count is out of bounds");
    }

    std::vector<uint32_t> indices(static_cast<size_t>(accessor.sparse.count), 0);
    for (int sparseIndex = 0; sparseIndex < accessor.sparse.count; ++sparseIndex)
    {
        const unsigned char* indexData = GetBufferViewDataPointer(
            model,
            indicesBufferView,
            accessor.sparse.indices.byteOffset + static_cast<size_t>(sparseIndex) * static_cast<size_t>(componentSize),
            static_cast<size_t>(componentSize));
        const uint32_t targetIndex = ReadIndexComponent(indexData, accessor.sparse.indices.componentType);
        if (targetIndex >= accessor.count)
        {
            throw std::runtime_error("glTF sparse accessor index is out of bounds");
        }
        indices[static_cast<size_t>(sparseIndex)] = targetIndex;
    }

    return indices;
}

std::vector<float> ReadAccessorFloatComponents(
    const tinygltf::Model& model,
    int accessorIndex,
    size_t expectedComponents)
{
    EnsureIndexInRange(static_cast<size_t>(accessorIndex), model.accessors.size(), "accessor");
    const tinygltf::Accessor& accessor = model.accessors[static_cast<size_t>(accessorIndex)];
    const int actualComponents = tinygltf::GetNumComponentsInType(static_cast<uint32_t>(accessor.type));
    if (actualComponents <= 0)
    {
        throw std::runtime_error("glTF accessor has an invalid element type");
    }

    std::vector<float> values(accessor.count * expectedComponents, 0.0f);
    if (static_cast<size_t>(actualComponents) < expectedComponents)
    {
        for (size_t elementIndex = 0; elementIndex < accessor.count; ++elementIndex)
        {
            for (size_t componentIndex = static_cast<size_t>(actualComponents); componentIndex < expectedComponents; ++componentIndex)
            {
                values[elementIndex * expectedComponents + componentIndex] =
                    componentIndex == 3 ? 1.0f : 0.0f;
            }
        }
    }

    const int componentSize = tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(accessor.componentType));
    if (componentSize <= 0)
    {
        throw std::runtime_error("glTF accessor has an invalid component size");
    }
    const size_t elementByteSize = static_cast<size_t>(componentSize) * static_cast<size_t>(actualComponents);

    if (accessor.bufferView >= 0)
    {
        EnsureIndexInRange(static_cast<size_t>(accessor.bufferView), model.bufferViews.size(), "buffer view");
        const tinygltf::BufferView& bufferView = model.bufferViews[static_cast<size_t>(accessor.bufferView)];
        for (size_t elementIndex = 0; elementIndex < accessor.count; ++elementIndex)
        {
            const unsigned char* elementData = GetAccessorDataPointer(model, accessor, bufferView, elementIndex, elementByteSize);
            for (size_t componentIndex = 0; componentIndex < expectedComponents; ++componentIndex)
            {
                if (static_cast<int>(componentIndex) >= actualComponents)
                {
                    values[elementIndex * expectedComponents + componentIndex] =
                        componentIndex == 3 ? 1.0f : 0.0f;
                    continue;
                }

                values[elementIndex * expectedComponents + componentIndex] = static_cast<float>(
                    ReadComponentAsDouble(
                        elementData + componentIndex * static_cast<size_t>(componentSize),
                        accessor.componentType,
                        accessor.normalized));
            }
        }
    }
    else if (!accessor.sparse.isSparse)
    {
        throw std::runtime_error("glTF accessor is missing buffer view data");
    }

    if (accessor.sparse.isSparse)
    {
        const std::vector<uint32_t> sparseIndices = ReadSparseAccessorIndices(model, accessor);
        EnsureIndexInRange(static_cast<size_t>(accessor.sparse.values.bufferView), model.bufferViews.size(), "buffer view");
        const tinygltf::BufferView& valuesBufferView =
            model.bufferViews[static_cast<size_t>(accessor.sparse.values.bufferView)];

        for (size_t sparseIndex = 0; sparseIndex < sparseIndices.size(); ++sparseIndex)
        {
            const unsigned char* elementData = GetBufferViewDataPointer(
                model,
                valuesBufferView,
                accessor.sparse.values.byteOffset + sparseIndex * elementByteSize,
                elementByteSize);
            const size_t targetElementIndex = sparseIndices[sparseIndex];

            for (size_t componentIndex = 0; componentIndex < expectedComponents; ++componentIndex)
            {
                if (static_cast<int>(componentIndex) >= actualComponents)
                {
                    values[targetElementIndex * expectedComponents + componentIndex] =
                        componentIndex == 3 ? 1.0f : 0.0f;
                    continue;
                }

                values[targetElementIndex * expectedComponents + componentIndex] = static_cast<float>(
                    ReadComponentAsDouble(
                        elementData + componentIndex * static_cast<size_t>(componentSize),
                        accessor.componentType,
                        accessor.normalized));
            }
        }
    }

    return values;
}

std::vector<uint32_t> ReadIndices(const tinygltf::Model& model, int accessorIndex)
{
    EnsureIndexInRange(static_cast<size_t>(accessorIndex), model.accessors.size(), "accessor");
    const tinygltf::Accessor& accessor = model.accessors[static_cast<size_t>(accessorIndex)];

    std::vector<uint32_t> indices(accessor.count, 0);
    const int componentSize = tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(accessor.componentType));
    if (componentSize <= 0)
    {
        throw std::runtime_error("glTF index accessor has an invalid component size");
    }
    const size_t elementByteSize = static_cast<size_t>(componentSize);

    if (accessor.bufferView >= 0)
    {
        EnsureIndexInRange(static_cast<size_t>(accessor.bufferView), model.bufferViews.size(), "buffer view");
        const tinygltf::BufferView& bufferView = model.bufferViews[static_cast<size_t>(accessor.bufferView)];
        for (size_t elementIndex = 0; elementIndex < accessor.count; ++elementIndex)
        {
            const unsigned char* elementData = GetAccessorDataPointer(model, accessor, bufferView, elementIndex, elementByteSize);
            indices[elementIndex] = ReadIndexComponent(elementData, accessor.componentType);
        }
    }
    else if (!accessor.sparse.isSparse)
    {
        throw std::runtime_error("glTF index accessor is missing buffer view data");
    }

    if (accessor.sparse.isSparse)
    {
        const std::vector<uint32_t> sparseIndices = ReadSparseAccessorIndices(model, accessor);
        EnsureIndexInRange(static_cast<size_t>(accessor.sparse.values.bufferView), model.bufferViews.size(), "buffer view");
        const tinygltf::BufferView& valuesBufferView =
            model.bufferViews[static_cast<size_t>(accessor.sparse.values.bufferView)];

        for (size_t sparseIndex = 0; sparseIndex < sparseIndices.size(); ++sparseIndex)
        {
            const unsigned char* elementData = GetBufferViewDataPointer(
                model,
                valuesBufferView,
                accessor.sparse.values.byteOffset + sparseIndex * elementByteSize,
                elementByteSize);
            indices[sparseIndices[sparseIndex]] = ReadIndexComponent(elementData, accessor.componentType);
        }
    }

    return indices;
}

glm::mat4 BuildNodeMatrix(const tinygltf::Node& node)
{
    if (node.matrix.size() == 16)
    {
        glm::mat4 matrix(1.0f);
        for (size_t column = 0; column < 4; ++column)
        {
            for (size_t row = 0; row < 4; ++row)
            {
                matrix[static_cast<glm::length_t>(column)][static_cast<glm::length_t>(row)] =
                    static_cast<float>(node.matrix[column * 4 + row]);
            }
        }
        return matrix;
    }

    glm::mat4 matrix(1.0f);
    if (node.translation.size() == 3)
    {
        matrix = glm::translate(
            matrix,
            glm::vec3(
                static_cast<float>(node.translation[0]),
                static_cast<float>(node.translation[1]),
                static_cast<float>(node.translation[2])));
    }
    if (node.rotation.size() == 4)
    {
        const glm::quat rotation(
            static_cast<float>(node.rotation[3]),
            static_cast<float>(node.rotation[0]),
            static_cast<float>(node.rotation[1]),
            static_cast<float>(node.rotation[2]));
        matrix *= glm::mat4_cast(rotation);
    }
    if (node.scale.size() == 3)
    {
        matrix = glm::scale(
            matrix,
            glm::vec3(
                static_cast<float>(node.scale[0]),
                static_cast<float>(node.scale[1]),
                static_cast<float>(node.scale[2])));
    }

    return matrix;
}

void ExpandBounds(const glm::vec3& position, LoadedModelData& modelData)
{
    if (!modelData.hasBounds)
    {
        modelData.minBounds = position;
        modelData.maxBounds = position;
        modelData.hasBounds = true;
        return;
    }

    modelData.minBounds = glm::min(modelData.minBounds, position);
    modelData.maxBounds = glm::max(modelData.maxBounds, position);
}

// True when the image is a plain companion file of the glTF, i.e. the only case
// where the loader keeps the URI and never looks at the decoded pixels. Embedded
// images (data: URIs, buffer views) and remote URLs are not: embedded ones are
// unpacked by UnpackEmbeddedTextures, which needs tinygltf to have decoded them.
bool IsExternalImageFileReference(const std::string& uri)
{
    return !uri.empty() && !uri.starts_with("data:") && uri.find("://") == std::string::npos;
}

// True when the image's pixels live inside the model file rather than in a
// companion file: a .glb bufferView, or a data: URI. A remote URI ("://") is
// neither embedded nor a local companion — the engine cannot fetch it, so it
// resolves to no texture at all.
bool IsEmbeddedImage(const tinygltf::Image& image)
{
    return image.uri.empty() || image.uri.starts_with("data:");
}

// The subdirectory embedded images are unpacked into, relative to the model.
constexpr const char* kUnpackedTextureDirectory = "textures";

// Returns the texture path relative to the model file's directory, or an
// absolute path if the URI was already absolute. Callers are responsible for
// resolving relative paths against the model's current location at load time.
std::string ResolveExternalImagePath(const std::filesystem::path& /*modelPath*/, const tinygltf::Image& image)
{
    if (!IsExternalImageFileReference(image.uri))
    {
        return {};
    }

    return std::filesystem::path(DecodeUriPath(image.uri)).lexically_normal().string();
}

// Writes one decoded image to disk as PNG. Returns false when the image
// carries nothing writable, which is not an error: the caller reports the
// resulting texture as missing.
bool WriteUnpackedImage(const tinygltf::Image& image, const std::filesystem::path& outputPath)
{
    if (image.image.empty() || image.width <= 0 || image.height <= 0 ||
        image.component <= 0 || image.component > 4)
    {
        return false;
    }
    if (image.bits > 8)
    {
        LOG_WARN(
            "Skipping embedded image '{}' because {}-bit textures are not yet supported.",
            outputPath.string(),
            image.bits);
        return false;
    }

    std::error_code existsEc;
    if (std::filesystem::exists(outputPath, existsEc) && !existsEc)
    {
        return true; // already unpacked; never overwrite
    }

    const int writeResult = stbi_write_png(
        outputPath.string().c_str(),
        image.width,
        image.height,
        image.component,
        image.image.data(),
        image.width * image.component);
    if (writeResult == 0)
    {
        throw std::runtime_error("Failed to unpack embedded glTF texture: " + outputPath.string());
    }
    return true;
}

std::string ResolveImagePath(
    const tinygltf::Model& model,
    const std::filesystem::path& modelPath,
    int textureIndex)
{
    if (textureIndex < 0)
    {
        return {};
    }

    EnsureIndexInRange(static_cast<size_t>(textureIndex), model.textures.size(), "texture");
    const tinygltf::Texture& texture = model.textures[static_cast<size_t>(textureIndex)];
    if (texture.source < 0)
    {
        return {};
    }

    EnsureIndexInRange(static_cast<size_t>(texture.source), model.images.size(), "image");
    const tinygltf::Image& image = model.images[static_cast<size_t>(texture.source)];

    if (const std::string externalPath = ResolveExternalImagePath(modelPath, image); !externalPath.empty())
    {
        return externalPath;
    }

    if (!IsEmbeddedImage(image))
    {
        // A remote URI the engine cannot fetch. Nothing to point at.
        LOG_WARN("Ignoring remote texture URI '{}' in '{}'", image.uri, modelPath.string());
        return {};
    }

    // Embedded: import unpacked this image into the bundle. Derive the same
    // name import wrote and confirm it is there, so a bundle imported before
    // unpacking existed degrades to an untextured material with one warning
    // rather than to a path that resolves to nothing.
    const std::string relativePath =
        (std::filesystem::path(kUnpackedTextureDirectory) /
         BuildEmbeddedTextureFileName(image, static_cast<size_t>(texture.source)))
            .generic_string();

    std::error_code ec;
    if (std::filesystem::exists(modelPath.parent_path() / relativePath, ec) && !ec)
    {
        return relativePath;
    }

    LOG_WARN(
        "'{}' has an embedded texture that was never unpacked (expected '{}'); re-import the model",
        modelPath.string(),
        relativePath);
    return {};
}

// KHR_texture_transform on a textureInfo, over the textureInfo's own texCoord (which the
// extension's texCoord, when present, overrides). The engine reads two UV sets; a texture asking
// for a third samples the second, with a warning.
TextureTransform ReadTextureTransform(int texCoord, const tinygltf::Value* transformExtension, const std::filesystem::path& modelPath)
{
    TextureTransform transform{};
    int set = texCoord;
    if (transformExtension != nullptr && transformExtension->IsObject())
    {
        const tinygltf::Value& t = *transformExtension;
        if (t.Has("offset") && t.Get("offset").IsArray() && t.Get("offset").ArrayLen() >= 2)
        {
            transform.offset[0] = static_cast<float>(t.Get("offset").Get(0).GetNumberAsDouble());
            transform.offset[1] = static_cast<float>(t.Get("offset").Get(1).GetNumberAsDouble());
        }
        if (t.Has("rotation") && t.Get("rotation").IsNumber())
        {
            transform.rotation = static_cast<float>(t.Get("rotation").GetNumberAsDouble());
        }
        if (t.Has("scale") && t.Get("scale").IsArray() && t.Get("scale").ArrayLen() >= 2)
        {
            transform.scale[0] = static_cast<float>(t.Get("scale").Get(0).GetNumberAsDouble());
            transform.scale[1] = static_cast<float>(t.Get("scale").Get(1).GetNumberAsDouble());
        }
        if (t.Has("texCoord") && t.Get("texCoord").IsInt())
        {
            set = t.Get("texCoord").GetNumberAsInt();
        }
    }
    if (set > 1)
    {
        LOG_WARN("'{}' has a texture on TEXCOORD_{}; the engine reads two UV sets and uses TEXCOORD_1", modelPath.string(), set);
    }
    transform.texCoord = set >= 1 ? 1u : 0u;
    return transform;
}

TextureTransform ReadTextureTransform(int texCoord, const tinygltf::ExtensionMap& extensions, const std::filesystem::path& modelPath)
{
    const auto found = extensions.find("KHR_texture_transform");
    return ReadTextureTransform(texCoord, found != extensions.end() ? &found->second : nullptr, modelPath);
}

// The sampler of the texture at textureIndex (glTF's textures[i].sampler), or the default.
TextureSampler ReadTextureSampler(const tinygltf::Model& model, int textureIndex)
{
    if (textureIndex < 0 || static_cast<size_t>(textureIndex) >= model.textures.size())
    {
        return TextureSampler{};
    }
    const int samplerIndex = model.textures[static_cast<size_t>(textureIndex)].sampler;
    if (samplerIndex < 0 || static_cast<size_t>(samplerIndex) >= model.samplers.size())
    {
        return TextureSampler{};
    }
    const tinygltf::Sampler& sampler = model.samplers[static_cast<size_t>(samplerIndex)];
    return TextureSamplerFromGltf(sampler.wrapS, sampler.wrapT, sampler.magFilter, sampler.minFilter);
}

ModelMaterialData BuildMaterialData(
    const tinygltf::Model& model,
    const tinygltf::Material& material,
    const std::filesystem::path& modelPath)
{
    ModelMaterialData materialData{};
    materialData.name = material.name;
    materialData.doubleSided = material.doubleSided;

    const auto& pbr = material.pbrMetallicRoughness;
    if (pbr.baseColorFactor.size() >= 4)
    {
        for (size_t index = 0; index < 4; ++index)
        {
            materialData.baseColor[index] = static_cast<float>(pbr.baseColorFactor[index]);
        }
    }
    materialData.baseColorTexturePath = ResolveImagePath(model, modelPath, pbr.baseColorTexture.index);
    const auto setTransform = [&](MaterialTextureSlot slot, const TextureTransform& transform)
    {
        materialData.textureTransforms[static_cast<size_t>(slot)] = transform;
    };
    // The glTF sampler of the texture a slot reads; the default without a texture or a sampler.
    const auto setSampler = [&](MaterialTextureSlot slot, int textureIndex)
    {
        materialData.textureSamplers[static_cast<size_t>(slot)] = ReadTextureSampler(model, textureIndex);
    };
    setTransform(MaterialTextureSlot::BaseColor, ReadTextureTransform(pbr.baseColorTexture.texCoord, pbr.baseColorTexture.extensions, modelPath));
    setSampler(MaterialTextureSlot::BaseColor, pbr.baseColorTexture.index);
    materialData.metallicFactor = static_cast<float>(pbr.metallicFactor);
    materialData.roughnessFactor = static_cast<float>(pbr.roughnessFactor);

    const std::string metallicRoughnessTexturePath =
        ResolveImagePath(model, modelPath, pbr.metallicRoughnessTexture.index);
    materialData.metallicTexturePath = metallicRoughnessTexturePath;
    materialData.roughnessTexturePath = metallicRoughnessTexturePath;
    // One glTF texture feeds both slots, so both take its transform.
    const TextureTransform metallicRoughnessTransform =
        ReadTextureTransform(pbr.metallicRoughnessTexture.texCoord, pbr.metallicRoughnessTexture.extensions, modelPath);
    setTransform(MaterialTextureSlot::Metallic, metallicRoughnessTransform);
    setTransform(MaterialTextureSlot::Roughness, metallicRoughnessTransform);
    setSampler(MaterialTextureSlot::Metallic, pbr.metallicRoughnessTexture.index);
    setSampler(MaterialTextureSlot::Roughness, pbr.metallicRoughnessTexture.index);

    if (material.normalTexture.index >= 0)
    {
        materialData.normalTexturePath = ResolveImagePath(model, modelPath, material.normalTexture.index);
        setTransform(MaterialTextureSlot::Normal, ReadTextureTransform(material.normalTexture.texCoord, material.normalTexture.extensions, modelPath));
        setSampler(MaterialTextureSlot::Normal, material.normalTexture.index);
        materialData.normalScale = static_cast<float>(material.normalTexture.scale);
    }

    if (material.occlusionTexture.index >= 0)
    {
        materialData.occlusionTexturePath = ResolveImagePath(model, modelPath, material.occlusionTexture.index);
        setTransform(
            MaterialTextureSlot::Occlusion, ReadTextureTransform(material.occlusionTexture.texCoord, material.occlusionTexture.extensions, modelPath));
        setSampler(MaterialTextureSlot::Occlusion, material.occlusionTexture.index);
        materialData.occlusionStrength = static_cast<float>(material.occlusionTexture.strength);
    }

    if (material.emissiveFactor.size() >= 3)
    {
        for (size_t index = 0; index < 3; ++index)
        {
            materialData.emissiveColor[index] = static_cast<float>(material.emissiveFactor[index]);
        }
    }
    if (material.emissiveTexture.index >= 0)
    {
        materialData.emissiveTexturePath = ResolveImagePath(model, modelPath, material.emissiveTexture.index);
        setTransform(MaterialTextureSlot::Emissive, ReadTextureTransform(material.emissiveTexture.texCoord, material.emissiveTexture.extensions, modelPath));
        setSampler(MaterialTextureSlot::Emissive, material.emissiveTexture.index);
        materialData.emissiveIntensity = 1.0f;
    }

    // KHR_materials_emissive_strength scales the emissive factor past glTF's [0, 1], which is
    // what emissiveIntensity already does. Only a non-negative number is a strength; anything else
    // keeps the default rather than darkening or inverting the emission.
    const auto emissiveStrength = material.extensions.find("KHR_materials_emissive_strength");
    if (emissiveStrength != material.extensions.end() && emissiveStrength->second.Has("emissiveStrength"))
    {
        const tinygltf::Value& strength = emissiveStrength->second.Get("emissiveStrength");
        if (strength.IsNumber() && strength.GetNumberAsDouble() >= 0.0)
        {
            materialData.emissiveIntensity = static_cast<float>(strength.GetNumberAsDouble());
        }
        else
        {
            LOG_WARN(
                "Ignoring an invalid KHR_materials_emissive_strength on material '{}' in '{}'",
                material.name,
                modelPath.string());
        }
    }

    // The texture a layer extension's textureInfo member points at, or "" when it has none.
    const auto readExtensionTexture = [&](const tinygltf::Value& extension, const char* name, MaterialTextureSlot slot) -> std::string
    {
        if (!extension.Has(name) || !extension.Get(name).IsObject() || !extension.Get(name).Has("index") ||
            !extension.Get(name).Get("index").IsInt())
        {
            return {};
        }
        const tinygltf::Value& info = extension.Get(name);
        const int texCoord = info.Has("texCoord") && info.Get("texCoord").IsInt() ? info.Get("texCoord").GetNumberAsInt() : 0;
        const tinygltf::Value* transform =
            info.Has("extensions") && info.Get("extensions").Has("KHR_texture_transform") ? &info.Get("extensions").Get("KHR_texture_transform")
                                                                                          : nullptr;
        setTransform(slot, ReadTextureTransform(texCoord, transform, modelPath));
        setSampler(slot, info.Get("index").GetNumberAsInt());
        return ResolveImagePath(model, modelPath, info.Get("index").GetNumberAsInt());
    };

    // KHR_materials_clearcoat. Absent members take the extension's defaults, 0.
    const auto clearcoat = material.extensions.find("KHR_materials_clearcoat");
    if (clearcoat != material.extensions.end())
    {
        const auto readUnitFactor = [&](const char* name)
        {
            if (!clearcoat->second.Has(name) || !clearcoat->second.Get(name).IsNumber())
            {
                return 0.0f;
            }
            return std::clamp(static_cast<float>(clearcoat->second.Get(name).GetNumberAsDouble()), 0.0f, 1.0f);
        };
        materialData.clearcoatFactor = readUnitFactor("clearcoatFactor");
        materialData.clearcoatRoughnessFactor = readUnitFactor("clearcoatRoughnessFactor");
        materialData.clearcoatTexturePath = readExtensionTexture(clearcoat->second, "clearcoatTexture", MaterialTextureSlot::Clearcoat);
        materialData.clearcoatRoughnessTexturePath = readExtensionTexture(clearcoat->second, "clearcoatRoughnessTexture", MaterialTextureSlot::ClearcoatRoughness);
        materialData.clearcoatNormalTexturePath = readExtensionTexture(clearcoat->second, "clearcoatNormalTexture", MaterialTextureSlot::ClearcoatNormal);
        if (clearcoat->second.Has("clearcoatNormalTexture") && clearcoat->second.Get("clearcoatNormalTexture").IsObject())
        {
            const tinygltf::Value& normalInfo = clearcoat->second.Get("clearcoatNormalTexture");
            if (normalInfo.Has("scale") && normalInfo.Get("scale").IsNumber())
            {
                materialData.clearcoatNormalScale = static_cast<float>(normalInfo.Get("scale").GetNumberAsDouble());
            }
        }
    }

    // KHR_materials_sheen. Absent members take the extension's defaults: black, 0.
    const auto sheen = material.extensions.find("KHR_materials_sheen");
    if (sheen != material.extensions.end())
    {
        if (sheen->second.Has("sheenColorFactor") && sheen->second.Get("sheenColorFactor").IsArray())
        {
            const tinygltf::Value& color = sheen->second.Get("sheenColorFactor");
            for (int index = 0; index < 3 && index < static_cast<int>(color.ArrayLen()); ++index)
            {
                if (color.Get(index).IsNumber())
                {
                    materialData.sheenColorFactor[index] =
                        std::clamp(static_cast<float>(color.Get(index).GetNumberAsDouble()), 0.0f, 1.0f);
                }
            }
        }
        if (sheen->second.Has("sheenRoughnessFactor") && sheen->second.Get("sheenRoughnessFactor").IsNumber())
        {
            materialData.sheenRoughnessFactor =
                std::clamp(static_cast<float>(sheen->second.Get("sheenRoughnessFactor").GetNumberAsDouble()), 0.0f, 1.0f);
        }
        materialData.sheenColorTexturePath = readExtensionTexture(sheen->second, "sheenColorTexture", MaterialTextureSlot::SheenColor);
        materialData.sheenRoughnessTexturePath = readExtensionTexture(sheen->second, "sheenRoughnessTexture", MaterialTextureSlot::SheenRoughness);
    }

    // KHR_materials_ior. Absent: 1.5. SanitizeIor turns an invalid index into the default.
    const auto ior = material.extensions.find("KHR_materials_ior");
    if (ior != material.extensions.end() && ior->second.Has("ior") && ior->second.Get("ior").IsNumber())
    {
        materialData.ior = SanitizeIor(static_cast<float>(ior->second.Get("ior").GetNumberAsDouble()));
    }

    // KHR_materials_specular. Absent members take the extension's defaults: strength 1, white.
    const auto specular = material.extensions.find("KHR_materials_specular");
    if (specular != material.extensions.end())
    {
        if (specular->second.Has("specularFactor") && specular->second.Get("specularFactor").IsNumber())
        {
            materialData.specularFactor =
                std::clamp(static_cast<float>(specular->second.Get("specularFactor").GetNumberAsDouble()), 0.0f, 1.0f);
        }
        if (specular->second.Has("specularColorFactor") && specular->second.Get("specularColorFactor").IsArray())
        {
            const tinygltf::Value& color = specular->second.Get("specularColorFactor");
            for (int index = 0; index < 3 && index < static_cast<int>(color.ArrayLen()); ++index)
            {
                if (color.Get(index).IsNumber())
                {
                    // Above 1 is allowed: the colour scales an F0 that is itself small.
                    materialData.specularColorFactor[index] = std::max(static_cast<float>(color.Get(index).GetNumberAsDouble()), 0.0f);
                }
            }
        }
        materialData.specularTexturePath = readExtensionTexture(specular->second, "specularTexture", MaterialTextureSlot::Specular);
        materialData.specularColorTexturePath = readExtensionTexture(specular->second, "specularColorTexture", MaterialTextureSlot::SpecularColor);
    }

    // KHR_materials_transmission and KHR_materials_volume. Absent members take the extensions'
    // defaults: transmission 0; thickness 0 (thin), no absorption (an infinite distance, stored as
    // 0), a white attenuation colour.
    const auto readExtensionNumber = [](const tinygltf::Value& extension, const char* name, float fallback)
    {
        return extension.Has(name) && extension.Get(name).IsNumber() ? static_cast<float>(extension.Get(name).GetNumberAsDouble()) : fallback;
    };
    if (const auto transmission = material.extensions.find("KHR_materials_transmission"); transmission != material.extensions.end())
    {
        materialData.transmissionFactor = std::clamp(readExtensionNumber(transmission->second, "transmissionFactor", 0.0f), 0.0f, 1.0f);
        materialData.transmissionTexturePath =
            readExtensionTexture(transmission->second, "transmissionTexture", MaterialTextureSlot::Transmission);
    }
    if (const auto volume = material.extensions.find("KHR_materials_volume"); volume != material.extensions.end())
    {
        materialData.thicknessFactor = std::max(readExtensionNumber(volume->second, "thicknessFactor", 0.0f), 0.0f);
        materialData.thicknessTexturePath = readExtensionTexture(volume->second, "thicknessTexture", MaterialTextureSlot::Thickness);
        const float distance = readExtensionNumber(volume->second, "attenuationDistance", 0.0f);
        materialData.attenuationDistance = std::isfinite(distance) && distance > 0.0f ? distance : 0.0f;
        if (volume->second.Has("attenuationColor") && volume->second.Get("attenuationColor").IsArray() &&
            volume->second.Get("attenuationColor").ArrayLen() >= 3)
        {
            const tinygltf::Value& color = volume->second.Get("attenuationColor");
            for (int index = 0; index < 3; ++index)
            {
                if (color.Get(index).IsNumber())
                {
                    materialData.attenuationColor[index] = std::clamp(static_cast<float>(color.Get(index).GetNumberAsDouble()), 0.0f, 1.0f);
                }
            }
        }
    }

    // KHR_materials_dispersion: 0 (none) when absent or negative.
    if (const auto dispersion = material.extensions.find("KHR_materials_dispersion"); dispersion != material.extensions.end())
    {
        materialData.dispersion = std::max(readExtensionNumber(dispersion->second, "dispersion", 0.0f), 0.0f);
    }
    // KHR_materials_diffuse_transmission (Release Candidate). Absent members take the extension's
    // defaults: factor 0, a white colour.
    if (const auto diffuseTransmission = material.extensions.find("KHR_materials_diffuse_transmission");
        diffuseTransmission != material.extensions.end())
    {
        const tinygltf::Value& extension = diffuseTransmission->second;
        materialData.diffuseTransmissionFactor = std::clamp(readExtensionNumber(extension, "diffuseTransmissionFactor", 0.0f), 0.0f, 1.0f);
        materialData.diffuseTransmissionTexturePath =
            readExtensionTexture(extension, "diffuseTransmissionTexture", MaterialTextureSlot::DiffuseTransmission);
        materialData.diffuseTransmissionColorTexturePath =
            readExtensionTexture(extension, "diffuseTransmissionColorTexture", MaterialTextureSlot::DiffuseTransmissionColor);
        if (extension.Has("diffuseTransmissionColorFactor") && extension.Get("diffuseTransmissionColorFactor").IsArray() &&
            extension.Get("diffuseTransmissionColorFactor").ArrayLen() >= 3)
        {
            const tinygltf::Value& color = extension.Get("diffuseTransmissionColorFactor");
            for (int index = 0; index < 3; ++index)
            {
                if (color.Get(index).IsNumber())
                {
                    materialData.diffuseTransmissionColor[index] = std::clamp(static_cast<float>(color.Get(index).GetNumberAsDouble()), 0.0f, 1.0f);
                }
            }
        }
    }

    // KHR_materials_iridescence. Absent members take the extension's defaults: factor 0, IOR 1.3,
    // thickness 100 to 400 nm.
    const auto iridescence = material.extensions.find("KHR_materials_iridescence");
    if (iridescence != material.extensions.end())
    {
        const auto readNumber = [&](const char* name, float fallback)
        {
            return iridescence->second.Has(name) && iridescence->second.Get(name).IsNumber()
                       ? static_cast<float>(iridescence->second.Get(name).GetNumberAsDouble())
                       : fallback;
        };
        materialData.iridescenceFactor = std::clamp(readNumber("iridescenceFactor", 0.0f), 0.0f, 1.0f);
        materialData.iridescenceIor = std::max(readNumber("iridescenceIor", 1.3f), 1.0f);
        materialData.iridescenceThicknessMinimum = std::max(readNumber("iridescenceThicknessMinimum", 100.0f), 0.0f);
        materialData.iridescenceThicknessMaximum = std::max(readNumber("iridescenceThicknessMaximum", 400.0f), 0.0f);
        materialData.iridescenceTexturePath = readExtensionTexture(iridescence->second, "iridescenceTexture", MaterialTextureSlot::Iridescence);
        materialData.iridescenceThicknessTexturePath = readExtensionTexture(iridescence->second, "iridescenceThicknessTexture", MaterialTextureSlot::IridescenceThickness);
    }

    // KHR_materials_anisotropy. Absent members take the extension's defaults: strength 0, rotation 0.
    const auto anisotropy = material.extensions.find("KHR_materials_anisotropy");
    if (anisotropy != material.extensions.end())
    {
        if (anisotropy->second.Has("anisotropyStrength") && anisotropy->second.Get("anisotropyStrength").IsNumber())
        {
            materialData.anisotropyStrength =
                std::clamp(static_cast<float>(anisotropy->second.Get("anisotropyStrength").GetNumberAsDouble()), 0.0f, 1.0f);
        }
        if (anisotropy->second.Has("anisotropyRotation") && anisotropy->second.Get("anisotropyRotation").IsNumber())
        {
            materialData.anisotropyRotation = static_cast<float>(anisotropy->second.Get("anisotropyRotation").GetNumberAsDouble());
        }
        materialData.anisotropyTexturePath = readExtensionTexture(anisotropy->second, "anisotropyTexture", MaterialTextureSlot::Anisotropy);
    }

    // KHR_materials_unlit has no members: its presence is the whole of it.
    materialData.unlit = material.extensions.find("KHR_materials_unlit") != material.extensions.end();

    const std::optional<MaterialAlphaMode> parsedAlphaMode =
        ParseMaterialAlphaMode(material.alphaMode);
    materialData.alphaMode = parsedAlphaMode.value_or(MaterialAlphaMode::Opaque);
    materialData.alphaCutoff = materialData.alphaMode == MaterialAlphaMode::Mask
                                   ? ClampMaterialAlphaValue(static_cast<float>(material.alphaCutoff), 0.5f)
                                   : 0.5f;
    materialData.opacity = 1.0f;
    return materialData;
}

uint32_t EnsureDefaultMaterial(LoadedModelData& modelData)
{
    if (modelData.materials.empty())
    {
        modelData.materials.push_back(ModelMaterialData{});
    }

    return 0;
}

std::vector<uint32_t> BuildTriangleIndices(std::span<const uint32_t> primitiveIndices, int mode)
{
    std::vector<uint32_t> triangles;
    switch (mode)
    {
    case kGltfModeTriangles:
        if (primitiveIndices.size() % 3 != 0)
        {
            throw std::runtime_error("glTF triangle primitive does not contain a multiple-of-three index count");
        }
        triangles.assign(primitiveIndices.begin(), primitiveIndices.end());
        return triangles;

    case kGltfModeTriangleStrip:
        if (primitiveIndices.size() < 3)
        {
            return triangles;
        }
        triangles.reserve((primitiveIndices.size() - 2) * 3);
        for (size_t index = 0; index + 2 < primitiveIndices.size(); ++index)
        {
            const uint32_t a = primitiveIndices[index];
            const uint32_t b = primitiveIndices[index + 1];
            const uint32_t c = primitiveIndices[index + 2];
            if (a == b || b == c || a == c)
            {
                continue;
            }

            if ((index % 2) == 0)
            {
                triangles.insert(triangles.end(), {a, b, c});
            }
            else
            {
                triangles.insert(triangles.end(), {b, a, c});
            }
        }
        return triangles;

    case kGltfModeTriangleFan:
        if (primitiveIndices.size() < 3)
        {
            return triangles;
        }
        triangles.reserve((primitiveIndices.size() - 2) * 3);
        for (size_t index = 1; index + 1 < primitiveIndices.size(); ++index)
        {
            const uint32_t a = primitiveIndices[0];
            const uint32_t b = primitiveIndices[index];
            const uint32_t c = primitiveIndices[index + 1];
            if (a == b || b == c || a == c)
            {
                continue;
            }

            triangles.insert(triangles.end(), {a, b, c});
        }
        return triangles;

    case kGltfModePoints:
    case kGltfModeLines:
    case kGltfModeLineLoop:
    case kGltfModeLineStrip:
        throw std::runtime_error("MiniEngine only imports glTF triangle meshes");

    default:
        throw std::runtime_error("glTF primitive uses an unsupported draw mode");
    }
}

std::string BuildSubmeshName(
    const tinygltf::Node& node,
    const tinygltf::Mesh& mesh,
    size_t primitiveIndex)
{
    if (!node.name.empty() && !mesh.name.empty())
    {
        return node.name + "/" + mesh.name + "/primitive_" + std::to_string(primitiveIndex);
    }
    if (!mesh.name.empty())
    {
        return mesh.name + "/primitive_" + std::to_string(primitiveIndex);
    }
    if (!node.name.empty())
    {
        return node.name + "/primitive_" + std::to_string(primitiveIndex);
    }

    return "primitive_" + std::to_string(primitiveIndex);
}

// The Khronos Sample Viewer's box for a primitive (getExtentsFromAccessor): the POSITION accessor's
// min/max corners through the node's transform, and the centre and half-diagonal of their bounds.
// Keeps the vertices' bounds when the accessor declares none or is normalized.
void SetViewerBounds(const tinygltf::Accessor& accessor, const glm::mat4& worldTransform, ModelSubmeshData& submesh)
{
    submesh.viewerBoundsCenter = submesh.boundsCenter;
    submesh.viewerBoundsRadius = submesh.boundsRadius;
    if (accessor.minValues.size() != 3 || accessor.maxValues.size() != 3 || accessor.normalized)
    {
        return;
    }
    const glm::vec3 minimum(accessor.minValues[0], accessor.minValues[1], accessor.minValues[2]);
    const glm::vec3 maximum(accessor.maxValues[0], accessor.maxValues[1], accessor.maxValues[2]);
    glm::vec3 boxMin(std::numeric_limits<float>::max());
    glm::vec3 boxMax(-std::numeric_limits<float>::max());
    for (int corner = 0; corner < 8; ++corner)
    {
        const glm::vec3 local((corner & 1) ? maximum.x : minimum.x, (corner & 2) ? maximum.y : minimum.y, (corner & 4) ? maximum.z : minimum.z);
        const glm::vec3 point = glm::vec3(worldTransform * glm::vec4(local, 1.0f));
        boxMin = glm::min(boxMin, point);
        boxMax = glm::max(boxMax, point);
    }
    submesh.viewerBoundsCenter = (boxMin + boxMax) * 0.5f;
    submesh.viewerBoundsRadius = glm::length(boxMax - boxMin) * 0.5f;
}

// KHR_materials_variants on the root: the variants' names, an unnamed one called by its index.
std::vector<std::string> ReadMaterialVariantNames(const tinygltf::Model& model)
{
    std::vector<std::string> names;
    const auto found = model.extensions.find("KHR_materials_variants");
    if (found == model.extensions.end() || !found->second.Has("variants") || !found->second.Get("variants").IsArray())
    {
        return names;
    }
    const tinygltf::Value& variants = found->second.Get("variants");
    for (size_t index = 0; index < variants.ArrayLen(); ++index)
    {
        const tinygltf::Value& variant = variants.Get(static_cast<int>(index));
        std::string name;
        if (variant.Has("name") && variant.Get("name").IsString())
        {
            name = variant.Get("name").Get<std::string>();
        }
        const bool blank = std::all_of(name.begin(), name.end(), [](unsigned char c)
                                       {
                                           return std::isspace(c) != 0;
                                       });
        names.push_back(blank ? "Variant " + std::to_string(index) : name);
    }
    return names;
}

// KHR_materials_variants on a primitive: one material per variant, starting from the primitive's
// own. A mapping with an out-of-range index is skipped; a variant mapped twice keeps its first.
void ReadVariantMappings(const tinygltf::Primitive& primitive, const LoadedModelData& modelData, ModelSubmeshData& submesh)
{
    if (modelData.materialVariants.empty())
    {
        return;
    }
    submesh.variantMaterialIndices.assign(modelData.materialVariants.size(), submesh.materialIndex);
    const auto found = primitive.extensions.find("KHR_materials_variants");
    if (found == primitive.extensions.end() || !found->second.Has("mappings") || !found->second.Get("mappings").IsArray())
    {
        return;
    }
    std::vector<bool> mapped(modelData.materialVariants.size(), false);
    const tinygltf::Value& mappings = found->second.Get("mappings");
    for (size_t mappingIndex = 0; mappingIndex < mappings.ArrayLen(); ++mappingIndex)
    {
        const tinygltf::Value& mapping = mappings.Get(static_cast<int>(mappingIndex));
        if (!mapping.Has("material") || !mapping.Get("material").IsNumber() || !mapping.Has("variants") ||
            !mapping.Get("variants").IsArray())
        {
            continue;
        }
        const double material = mapping.Get("material").GetNumberAsDouble();
        if (material < 0.0 || material >= static_cast<double>(modelData.materials.size()))
        {
            LOG_WARN("KHR_materials_variants maps to material {}, which the model does not have; skipped", material);
            continue;
        }
        const tinygltf::Value& variants = mapping.Get("variants");
        for (size_t index = 0; index < variants.ArrayLen(); ++index)
        {
            const tinygltf::Value& variant = variants.Get(static_cast<int>(index));
            const double variantIndex = variant.IsNumber() ? variant.GetNumberAsDouble() : -1.0;
            if (variantIndex < 0.0 || variantIndex >= static_cast<double>(mapped.size()))
            {
                LOG_WARN("KHR_materials_variants maps variant {}, which the model does not have; skipped", variantIndex);
                continue;
            }
            const size_t slot = static_cast<size_t>(variantIndex);
            if (!mapped[slot])
            {
                mapped[slot] = true;
                submesh.variantMaterialIndices[slot] = static_cast<uint32_t>(material);
            }
        }
    }
}

void AppendPrimitive(
    const tinygltf::Model& model,
    const tinygltf::Node& node,
    const tinygltf::Mesh& mesh,
    const tinygltf::Primitive& primitive,
    size_t primitiveIndex,
    const glm::mat4& worldTransform,
    LoadedModelData& modelData)
{
    const auto positionIt = primitive.attributes.find("POSITION");
    if (positionIt == primitive.attributes.end())
    {
        throw std::runtime_error("glTF primitive is missing POSITION data");
    }

    const std::vector<float> positions = ReadAccessorFloatComponents(model, positionIt->second, 3);
    const size_t vertexCount = positions.size() / 3;
    if (vertexCount == 0)
    {
        return;
    }

    std::vector<float> normals;
    std::vector<float> tangents;
    std::vector<float> texCoords;
    std::vector<float> colors;

    if (const auto it = primitive.attributes.find("NORMAL"); it != primitive.attributes.end())
    {
        normals = ReadAccessorFloatComponents(model, it->second, 3);
        if (normals.size() / 3 != vertexCount)
        {
            throw std::runtime_error("glTF NORMAL accessor count does not match POSITION accessor count");
        }
    }
    if (const auto it = primitive.attributes.find("TANGENT"); it != primitive.attributes.end())
    {
        tangents = ReadAccessorFloatComponents(model, it->second, 4);
        if (tangents.size() / 4 != vertexCount)
        {
            throw std::runtime_error("glTF TANGENT accessor count does not match POSITION accessor count");
        }
    }
    if (const auto it = primitive.attributes.find("TEXCOORD_0"); it != primitive.attributes.end())
    {
        texCoords = ReadAccessorFloatComponents(model, it->second, 2);
        if (texCoords.size() / 2 != vertexCount)
        {
            throw std::runtime_error("glTF TEXCOORD_0 accessor count does not match POSITION accessor count");
        }
    }
    std::vector<float> texCoords1;
    if (const auto it = primitive.attributes.find("TEXCOORD_1"); it != primitive.attributes.end())
    {
        texCoords1 = ReadAccessorFloatComponents(model, it->second, 2);
        if (texCoords1.size() / 2 != vertexCount)
        {
            throw std::runtime_error("glTF TEXCOORD_1 accessor count does not match POSITION accessor count");
        }
    }
    if (const auto it = primitive.attributes.find("COLOR_0"); it != primitive.attributes.end())
    {
        colors = ReadAccessorFloatComponents(model, it->second, 4);
        if (colors.size() / 4 != vertexCount)
        {
            throw std::runtime_error("glTF COLOR_0 accessor count does not match POSITION accessor count");
        }
    }

    ModelSubmeshData submeshData{};
    submeshData.name = BuildSubmeshName(node, mesh, primitiveIndex);
    if (primitive.material >= 0)
    {
        EnsureIndexInRange(static_cast<size_t>(primitive.material), modelData.materials.size(), "material");
        submeshData.materialIndex = static_cast<uint32_t>(primitive.material);
    }
    else
    {
        submeshData.materialIndex = EnsureDefaultMaterial(modelData);
    }
    ReadVariantMappings(primitive, modelData, submeshData);
    submeshData.hasTexCoords = !texCoords.empty();
    submeshData.hasNormals = !normals.empty();
    submeshData.hasTangents = !tangents.empty();
    submeshData.mesh.vertices.resize(vertexCount);

    const glm::mat3 normalMatrix = glm::inverseTranspose(glm::mat3(worldTransform));
    const float tangentHandednessScale = glm::determinant(glm::mat3(worldTransform)) < 0.0f ? -1.0f : 1.0f;

    for (size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
    {
        Vertex& vertex = submeshData.mesh.vertices[vertexIndex];
        vertex.color[0] = 1.0f;
        vertex.color[1] = 1.0f;
        vertex.color[2] = 1.0f;
        vertex.tangent[3] = 1.0f;

        const glm::vec4 position = worldTransform * glm::vec4(
                                                        positions[vertexIndex * 3 + 0],
                                                        positions[vertexIndex * 3 + 1],
                                                        positions[vertexIndex * 3 + 2],
                                                        1.0f);
        vertex.position[0] = position.x;
        vertex.position[1] = position.y;
        vertex.position[2] = position.z;
        ExpandBounds(glm::vec3(position), modelData);

        if (!texCoords.empty())
        {
            vertex.texCoord[0] = texCoords[vertexIndex * 2 + 0];
            vertex.texCoord[1] = texCoords[vertexIndex * 2 + 1];
        }
        if (!texCoords1.empty())
        {
            vertex.texCoord1[0] = texCoords1[vertexIndex * 2 + 0];
            vertex.texCoord1[1] = texCoords1[vertexIndex * 2 + 1];
        }

        if (!colors.empty())
        {
            vertex.color[0] = colors[vertexIndex * 4 + 0];
            vertex.color[1] = colors[vertexIndex * 4 + 1];
            vertex.color[2] = colors[vertexIndex * 4 + 2];
        }

        if (!normals.empty())
        {
            glm::vec3 normal = glm::normalize(normalMatrix * glm::vec3(
                                                                 normals[vertexIndex * 3 + 0],
                                                                 normals[vertexIndex * 3 + 1],
                                                                 normals[vertexIndex * 3 + 2]));
            if (!std::isfinite(normal.x) || !std::isfinite(normal.y) || !std::isfinite(normal.z))
            {
                normal = glm::vec3(0.0f, 1.0f, 0.0f);
            }
            vertex.normal[0] = normal.x;
            vertex.normal[1] = normal.y;
            vertex.normal[2] = normal.z;
        }

        if (!tangents.empty())
        {
            glm::vec3 tangent = glm::normalize(glm::mat3(worldTransform) * glm::vec3(
                                                                               tangents[vertexIndex * 4 + 0],
                                                                               tangents[vertexIndex * 4 + 1],
                                                                               tangents[vertexIndex * 4 + 2]));
            if (!std::isfinite(tangent.x) || !std::isfinite(tangent.y) || !std::isfinite(tangent.z))
            {
                tangent = glm::vec3(1.0f, 0.0f, 0.0f);
            }
            vertex.tangent[0] = tangent.x;
            vertex.tangent[1] = tangent.y;
            vertex.tangent[2] = tangent.z;
            vertex.tangent[3] = tangents[vertexIndex * 4 + 3] * tangentHandednessScale;
        }
    }

    std::vector<uint32_t> primitiveIndices;
    if (primitive.indices >= 0)
    {
        primitiveIndices = ReadIndices(model, primitive.indices);
    }
    else
    {
        primitiveIndices.resize(vertexCount);
        for (size_t index = 0; index < vertexCount; ++index)
        {
            primitiveIndices[index] = static_cast<uint32_t>(index);
        }
    }

    submeshData.mesh.indices = BuildTriangleIndices(
        primitiveIndices,
        primitive.mode >= 0 ? primitive.mode : kGltfModeTriangles);

    if (tangentHandednessScale < 0.0f)
    {
        // The node's world transform mirrors the geometry (negative determinant), which already
        // got baked into the vertex positions above. That mirroring reverses the apparent winding
        // of every triangle, so flip it back to CCW here to match the engine's backface-culling
        // convention (see VulkanPipelineSet's frontFace/cullMode setup).
        for (size_t index = 0; index + 2 < submeshData.mesh.indices.size(); index += 3)
        {
            std::swap(submeshData.mesh.indices[index + 1], submeshData.mesh.indices[index + 2]);
        }
    }

    for (uint32_t index : submeshData.mesh.indices)
    {
        if (index >= submeshData.mesh.vertices.size())
        {
            throw std::runtime_error("glTF primitive index is out of bounds");
        }
    }

    ModelPostProcess::FinalizeSubmeshData(submeshData);
    SetViewerBounds(model.accessors[static_cast<size_t>(positionIt->second)], worldTransform, submeshData);
    submeshData.nodeScale = glm::vec3(
        glm::length(glm::vec3(worldTransform[0])),
        glm::length(glm::vec3(worldTransform[1])),
        glm::length(glm::vec3(worldTransform[2])));
    if (submeshData.mesh.IsValid())
    {
        modelData.submeshes.push_back(std::move(submeshData));
    }
}

// Load-fraction milestones for the overall progress reported to the UI.
constexpr float kProgressParseStarted = 0.02f;
constexpr float kProgressParseDone = 0.55f;
constexpr float kProgressMaterialsDone = 0.60f;

// tinygltf decodes every referenced image inside its single parse call. The
// image-loader hook overrides that: companion texture files are left undecoded
// (see below), and the hook doubles as the only place where the UI can be told
// that the parse is progressing. The total image count is unknown mid-parse, so
// the fraction approaches the parse-done milestone asymptotically.
struct GltfParseProgressContext
{
    const ModelLoadProgressCallback& callback;
    int imagesLoaded = 0;
};

bool LoadGltfImageData(
    tinygltf::Image* image,
    const int imageIndex,
    std::string* error,
    std::string* warning,
    int requestedWidth,
    int requestedHeight,
    const unsigned char* bytes,
    int size,
    void* userData)
{
    auto* context = static_cast<GltfParseProgressContext*>(userData);
    if (context != nullptr && context->callback)
    {
        ++context->imagesLoaded;
        const float imageCount = static_cast<float>(context->imagesLoaded);
        const float asymptoticFraction = imageCount / (imageCount + 4.0f);
        context->callback(
            kProgressParseStarted + (kProgressParseDone - kProgressParseStarted) * asymptoticFraction);
    }

    // A texture stored as a companion file keeps only its path in the loaded model
    // (ResolveExternalImagePath), and the renderer decodes those files in parallel
    // when it uploads them. Decoding them here as well would be the same work done
    // twice, the second time serialized on this single parse thread - which is most
    // of the parse cost for an asset like Sponza (~70 external PNGs). tinygltf fills
    // in image->uri before calling this hook for exactly that case, so an empty uri
    // still falls through to the decode below; leaving image->image empty is safe
    // because nothing reads the pixels of an image that resolves to a path.
    if (image != nullptr && IsExternalImageFileReference(image->uri))
    {
        return true;
    }

    // Installing a custom image loader bypasses TinyGLTF::SetPreserveImageChannels,
    // so the equivalent option must be forwarded to the default decoder here.
    tinygltf::LoadImageDataOption imageDataOption;
    imageDataOption.preserve_channels = true;
    return tinygltf::LoadImageData(
        image, imageIndex, error, warning, requestedWidth, requestedHeight, bytes, size, &imageDataOption);
}

struct GltfLoadProgressTracker
{
    const ModelLoadProgressCallback& callback;
    size_t processedPrimitives = 0;
    size_t estimatedTotalPrimitives = 0;

    void Report(float fraction) const
    {
        if (callback)
        {
            callback(std::clamp(fraction, 0.0f, 1.0f));
        }
    }

    void ReportPrimitiveProcessed()
    {
        ++processedPrimitives;
        if (!callback || estimatedTotalPrimitives == 0)
        {
            return;
        }

        // Instanced meshes can make the processed count exceed the estimate; clamp.
        const float primitiveFraction = std::min(
            1.0f,
            static_cast<float>(processedPrimitives) / static_cast<float>(estimatedTotalPrimitives));
        Report(kProgressMaterialsDone + (1.0f - kProgressMaterialsDone) * primitiveFraction);
    }
};

void TraverseNode(
    const tinygltf::Model& model,
    int nodeIndex,
    const glm::mat4& parentTransform,
    LoadedModelData& modelData,
    std::unordered_set<int>& visitedNodes,
    GltfLoadProgressTracker& progressTracker)
{
    if (!visitedNodes.insert(nodeIndex).second)
    {
        throw std::runtime_error("glTF scene graph contains a recursive node reference");
    }

    EnsureIndexInRange(static_cast<size_t>(nodeIndex), model.nodes.size(), "node");
    const tinygltf::Node& node = model.nodes[static_cast<size_t>(nodeIndex)];
    const glm::mat4 worldTransform = parentTransform * BuildNodeMatrix(node);

    if (node.mesh >= 0)
    {
        EnsureIndexInRange(static_cast<size_t>(node.mesh), model.meshes.size(), "mesh");
        const tinygltf::Mesh& mesh = model.meshes[static_cast<size_t>(node.mesh)];
        for (size_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex)
        {
            AppendPrimitive(
                model,
                node,
                mesh,
                mesh.primitives[primitiveIndex],
                primitiveIndex,
                worldTransform,
                modelData);
            progressTracker.ReportPrimitiveProcessed();
        }
    }

    for (int childIndex : node.children)
    {
        TraverseNode(model, childIndex, worldTransform, modelData, visitedNodes, progressTracker);
    }
}

LoadedModelData BuildLoadedModelData(
    const tinygltf::Model& tinyModel,
    const std::filesystem::path& modelPath,
    const ModelLoadProgressCallback& progress)
{
    GltfLoadProgressTracker progressTracker{progress};
    for (const tinygltf::Mesh& mesh : tinyModel.meshes)
    {
        progressTracker.estimatedTotalPrimitives += mesh.primitives.size();
    }

    LoadedModelData modelData{};
    modelData.materials.reserve(tinyModel.materials.size());
    for (const tinygltf::Material& material : tinyModel.materials)
    {
        modelData.materials.push_back(BuildMaterialData(tinyModel, material, modelPath));
    }
    modelData.materialVariants = ReadMaterialVariantNames(tinyModel);
    progressTracker.Report(kProgressMaterialsDone);

    std::unordered_set<int> visitedNodes;
    if (!tinyModel.scenes.empty())
    {
        const int sceneIndex = tinyModel.defaultScene >= 0 ? tinyModel.defaultScene : 0;
        EnsureIndexInRange(static_cast<size_t>(sceneIndex), tinyModel.scenes.size(), "scene");
        const tinygltf::Scene& scene = tinyModel.scenes[static_cast<size_t>(sceneIndex)];
        for (int rootNodeIndex : scene.nodes)
        {
            TraverseNode(tinyModel, rootNodeIndex, glm::mat4(1.0f), modelData, visitedNodes, progressTracker);
        }
    }
    else
    {
        std::vector<bool> isChild(tinyModel.nodes.size(), false);
        for (const tinygltf::Node& node : tinyModel.nodes)
        {
            for (int childIndex : node.children)
            {
                EnsureIndexInRange(static_cast<size_t>(childIndex), tinyModel.nodes.size(), "node");
                isChild[static_cast<size_t>(childIndex)] = true;
            }
        }

        bool traversedAnyRoot = false;
        for (size_t nodeIndex = 0; nodeIndex < tinyModel.nodes.size(); ++nodeIndex)
        {
            if (isChild[nodeIndex])
            {
                continue;
            }

            TraverseNode(tinyModel, static_cast<int>(nodeIndex), glm::mat4(1.0f), modelData, visitedNodes, progressTracker);
            traversedAnyRoot = true;
        }

        if (!traversedAnyRoot)
        {
            for (size_t nodeIndex = 0; nodeIndex < tinyModel.nodes.size(); ++nodeIndex)
            {
                TraverseNode(tinyModel, static_cast<int>(nodeIndex), glm::mat4(1.0f), modelData, visitedNodes, progressTracker);
            }
        }
    }

    if (modelData.submeshes.empty())
    {
        throw std::runtime_error("glTF scene did not contain renderable triangle mesh data: " + modelPath.string());
    }

    if (modelData.materials.empty())
    {
        modelData.materials.push_back(ModelMaterialData{});
    }

    return modelData;
}
}

namespace
{
// The extensions this loader implements. A model that requires another fails to import rather than
// drawing wrong; one that only uses another loads, with a warning.
constexpr std::array<std::string_view, 16> kImplementedExtensions = {
    "KHR_materials_anisotropy",
    "KHR_materials_clearcoat",
    "KHR_materials_diffuse_transmission",
    "KHR_materials_dispersion",
    "KHR_materials_emissive_strength",
    "KHR_materials_ior",
    "KHR_materials_iridescence",
    "KHR_materials_sheen",
    "KHR_materials_specular",
    "KHR_materials_transmission",
    "KHR_materials_unlit",
    "KHR_materials_variants",
    "KHR_materials_volume",
    "KHR_mesh_quantization",
    "KHR_texture_transform",
    "KHR_xmp_json_ld"};

bool IsImplementedExtension(const std::string& name)
{
    return std::find(kImplementedExtensions.begin(), kImplementedExtensions.end(), name) != kImplementedExtensions.end();
}

void CheckExtensions(const tinygltf::Model& model, const std::filesystem::path& modelPath)
{
    std::string missing;
    for (const std::string& name : model.extensionsRequired)
    {
        if (!IsImplementedExtension(name))
        {
            missing += (missing.empty() ? "" : ", ") + name;
        }
    }
    if (!missing.empty())
    {
        throw std::runtime_error("glTF model '" + modelPath.string() + "' requires extensions MiniEngine does not implement: " + missing);
    }
    for (const std::string& name : model.extensionsUsed)
    {
        if (!IsImplementedExtension(name))
        {
            LOG_WARN("glTF model '{}' uses {}, which MiniEngine ignores", modelPath.string(), name);
        }
    }
}
}

LoadedModelData GltfModelLoader::LoadModel(const std::string& path, const ModelLoadProgressCallback& progress)
{
    const std::filesystem::path modelPath = std::filesystem::path(path).lexically_normal();
    const std::string extension = ToLowerCopy(modelPath.extension().string());

    if (progress)
    {
        progress(kProgressParseStarted);
    }

    tinygltf::TinyGLTF loader;
    loader.SetPreserveImageChannels(true);

    // Always installed, with or without a progress callback: the hook is also what
    // keeps tinygltf from decoding companion texture files the loader never reads.
    GltfParseProgressContext parseProgressContext{progress};
    loader.SetImageLoader(&LoadGltfImageData, &parseProgressContext);

    tinygltf::Model tinyModel;
    std::string warnings;
    std::string errors;
    bool loaded = false;

    if (extension == ".glb")
    {
        loaded = loader.LoadBinaryFromFile(&tinyModel, &errors, &warnings, modelPath.string());
    }
    else if (extension == ".gltf")
    {
        loaded = loader.LoadASCIIFromFile(&tinyModel, &errors, &warnings, modelPath.string());
    }
    else
    {
        throw std::runtime_error("Unsupported glTF extension: " + modelPath.string());
    }

    if (!warnings.empty())
    {
        LOG_WARN("tinygltf warnings for '{}': {}", modelPath.string(), warnings);
    }
    if (!loaded)
    {
        throw std::runtime_error(
            "Failed to load glTF model '" + modelPath.string() + "'" +
            (errors.empty() ? std::string{} : ": " + errors));
    }

    CheckExtensions(tinyModel, modelPath);

    if (progress)
    {
        progress(kProgressParseDone);
    }

    return BuildLoadedModelData(tinyModel, modelPath, progress);
}

void GltfModelLoader::UnpackEmbeddedTextures(const std::filesystem::path& modelPath)
{
    const std::string extension = ToLowerCopy(modelPath.extension().string());

    // The same image hook as a load, without progress: companion files are left undecoded. Only
    // embedded images are unpacked, and a companion stb cannot read (an .exr) must not fail the
    // import. The hook forwards the preserve-channels option itself.
    tinygltf::TinyGLTF loader;
    loader.SetImageLoader(&LoadGltfImageData, nullptr);

    tinygltf::Model model;
    std::string warnings;
    std::string errors;
    bool loaded = false;

    if (extension == ".glb")
    {
        loaded = loader.LoadBinaryFromFile(&model, &errors, &warnings, modelPath.string());
    }
    else if (extension == ".gltf")
    {
        loaded = loader.LoadASCIIFromFile(&model, &errors, &warnings, modelPath.string());
    }
    else
    {
        return; // nothing else carries embedded glTF images
    }

    if (!loaded)
    {
        throw std::runtime_error(
            "Failed to parse '" + modelPath.string() + "' while unpacking embedded textures" +
            (errors.empty() ? std::string{} : ": " + errors));
    }

    const std::filesystem::path textureDirectory =
        modelPath.parent_path() / kUnpackedTextureDirectory;

    size_t unpacked = 0;
    for (size_t imageIndex = 0; imageIndex < model.images.size(); ++imageIndex)
    {
        const tinygltf::Image& image = model.images[imageIndex];
        if (!IsEmbeddedImage(image))
        {
            continue;
        }

        std::error_code mkdirEc;
        std::filesystem::create_directories(textureDirectory, mkdirEc);
        if (mkdirEc)
        {
            throw std::runtime_error(
                "Failed to create '" + textureDirectory.string() + "': " + mkdirEc.message());
        }

        const std::filesystem::path outputPath =
            textureDirectory / BuildEmbeddedTextureFileName(image, imageIndex);
        if (WriteUnpackedImage(image, outputPath))
        {
            ++unpacked;
        }
    }

    if (unpacked > 0)
    {
        LOG_INFO("Unpacked {} embedded texture(s) for '{}'", unpacked, modelPath.string());
    }
}

std::filesystem::path GltfModelLoader::CopyWithSortedReferences(
    const std::filesystem::path& gltfPath,
    const std::filesystem::path& targetDirectory)
{
    std::ifstream file(gltfPath, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("Failed to open glTF file: " + gltfPath.string());
    }

    // ordered_json keeps the original key order so the rewritten file stays
    // diffable against its source.
    nlohmann::ordered_json document = nlohmann::ordered_json::parse(file);

    const std::filesystem::path sourceDir = gltfPath.parent_path();
    std::unordered_map<std::string, std::string> rewrittenUris; // decoded source URI -> new encoded URI
    std::unordered_set<std::string> claimedRelPaths;            // destination-relative paths already assigned

    const auto processArray = [&](const char* arrayName, const char* subdirName)
    {
        const auto arrayIt = document.find(arrayName);
        if (arrayIt == document.end() || !arrayIt->is_array())
        {
            return;
        }
        for (nlohmann::ordered_json& element : *arrayIt)
        {
            const auto uriIt = element.find("uri");
            if (uriIt == element.end() || !uriIt->is_string())
            {
                continue;
            }
            const std::string uri = uriIt->get<std::string>();
            if (uri.starts_with("data:"))
            {
                continue;
            }

            const std::string decoded = DecodeUriPath(uri);
            if (const auto rewrittenIt = rewrittenUris.find(decoded); rewrittenIt != rewrittenUris.end())
            {
                *uriIt = rewrittenIt->second;
                continue;
            }

            const std::filesystem::path relPath = std::filesystem::path(decoded).lexically_normal();
            if (relPath.empty() || relPath.is_absolute() || relPath.begin()->string() == "..")
            {
                LOG_WARN(
                    "Reference '{}' in '{}' escapes the model folder; left as-is and not copied",
                    uri, gltfPath.string());
                continue;
            }

            const std::filesystem::path companionSrc = sourceDir / relPath;
            std::error_code fileEc;
            if (!std::filesystem::is_regular_file(companionSrc, fileEc) || fileEc)
            {
                LOG_WARN("Referenced file does not exist, not copied: {}", companionSrc.string());
                continue;
            }

            // Claim a flat name inside the sorting subfolder; distinct source
            // files that share a filename get a numeric suffix.
            const std::string stem = companionSrc.stem().string();
            const std::string extension = companionSrc.extension().string();
            std::string newRel = std::string(subdirName) + "/" + stem + extension;
            for (int suffix = 1; claimedRelPaths.count(newRel) > 0; ++suffix)
            {
                newRel = std::string(subdirName) + "/" + stem + "_" + std::to_string(suffix) + extension;
            }
            claimedRelPaths.insert(newRel);

            const std::filesystem::path companionDst = targetDirectory / std::filesystem::path(newRel);
            std::error_code copyEc;
            std::filesystem::create_directories(companionDst.parent_path(), copyEc);
            if (!copyEc)
            {
                std::filesystem::copy_file(companionSrc, companionDst, copyEc);
            }
            if (copyEc)
            {
                // The original URI would resolve against the new folder, where
                // the file is not: fail rather than import a broken model.
                throw std::runtime_error(
                    "Failed to copy '" + companionSrc.string() + "' to '" + companionDst.string() +
                    "': " + copyEc.message());
            }
            LOG_INFO("Copied companion: {} -> {}", companionSrc.string(), companionDst.string());

            const std::string newUri = EncodeUriPath(newRel);
            *uriIt = newUri;
            rewrittenUris.emplace(decoded, newUri);
        }
    };

    processArray("buffers", "buffers");
    processArray("images", "textures");

    const std::filesystem::path outPath = targetDirectory / gltfPath.filename();
    std::error_code existsEc;
    if (std::filesystem::exists(outPath, existsEc))
    {
        // Never clobber, and never report the old file as the new import: the
        // caller resolves conflicts before copying.
        throw std::runtime_error("Import target already exists: " + outPath.string());
    }

    std::ofstream out(outPath, std::ios::binary);
    if (!out)
    {
        throw std::runtime_error("Failed to write imported glTF: " + outPath.string());
    }
    out << document.dump(2);
    return outPath;
}
}
