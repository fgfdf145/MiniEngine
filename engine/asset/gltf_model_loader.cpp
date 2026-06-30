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
#include <log/log.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

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

std::string BuildCacheKey(const std::filesystem::path& modelPath)
{
    const std::string normalizedPath = modelPath.lexically_normal().string();
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char character : normalizedPath)
    {
        hash ^= static_cast<uint64_t>(character);
        hash *= 1099511628211ull;
    }

    std::ostringstream stream;
    stream << std::hex << hash;
    return stream.str();
}

std::filesystem::path BuildEmbeddedTextureCacheDirectory(const std::filesystem::path& modelPath)
{
    const std::string folderName =
        SanitizeFileName(modelPath.stem().string()) + "_" + BuildCacheKey(modelPath);
    return std::filesystem::path(MINIENGINE_PROJECT_DIR) / ".cache" / "tinygltf" / folderName;
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
    size_t elementByteSize
)
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
    size_t byteSize
)
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
    const tinygltf::Accessor& accessor
)
{
    if (!accessor.sparse.isSparse)
    {
        return {};
    }

    EnsureIndexInRange(static_cast<size_t>(accessor.sparse.indices.bufferView), model.bufferViews.size(), "buffer view");
    const tinygltf::BufferView& indicesBufferView =
        model.bufferViews[static_cast<size_t>(accessor.sparse.indices.bufferView)];
    const int componentSize = tinygltf::GetComponentSizeInBytes(
        static_cast<uint32_t>(accessor.sparse.indices.componentType)
    );
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
            static_cast<size_t>(componentSize)
        );
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
    size_t expectedComponents
)
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
                        accessor.normalized
                    )
                );
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
                elementByteSize
            );
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
                        accessor.normalized
                    )
                );
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
                elementByteSize
            );
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
                static_cast<float>(node.translation[2])
            )
        );
    }
    if (node.rotation.size() == 4)
    {
        const glm::quat rotation(
            static_cast<float>(node.rotation[3]),
            static_cast<float>(node.rotation[0]),
            static_cast<float>(node.rotation[1]),
            static_cast<float>(node.rotation[2])
        );
        matrix *= glm::mat4_cast(rotation);
    }
    if (node.scale.size() == 3)
    {
        matrix = glm::scale(
            matrix,
            glm::vec3(
                static_cast<float>(node.scale[0]),
                static_cast<float>(node.scale[1]),
                static_cast<float>(node.scale[2])
            )
        );
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

// Returns the texture path relative to the model file's directory, or an
// absolute path if the URI was already absolute. Callers are responsible for
// resolving relative paths against the model's current location at load time.
std::string ResolveExternalImagePath(const std::filesystem::path& /*modelPath*/, const tinygltf::Image& image)
{
    if (image.uri.empty() || image.uri.starts_with("data:"))
    {
        return {};
    }
    if (image.uri.find("://") != std::string::npos)
    {
        return {};
    }

    return std::filesystem::path(DecodeUriPath(image.uri)).lexically_normal().string();
}

std::string ExportEmbeddedImage(
    const std::filesystem::path& modelPath,
    const tinygltf::Image& image,
    size_t imageIndex
)
{
    if (image.image.empty() || image.width <= 0 || image.height <= 0 || image.component <= 0 || image.component > 4)
    {
        return {};
    }
    if (image.bits > 8)
    {
        LOG_WARN(
            "Skipping embedded image export for '{}' because {}-bit textures are not yet supported.",
            modelPath.string(),
            image.bits
        );
        return {};
    }

    const std::filesystem::path cacheDirectory = BuildEmbeddedTextureCacheDirectory(modelPath);
    std::filesystem::create_directories(cacheDirectory);
    const std::filesystem::path outputPath = cacheDirectory / BuildEmbeddedTextureFileName(image, imageIndex);

    if (!std::filesystem::exists(outputPath))
    {
        const int writeResult = stbi_write_png(
            outputPath.string().c_str(),
            image.width,
            image.height,
            image.component,
            image.image.data(),
            image.width * image.component
        );
        if (writeResult == 0)
        {
            throw std::runtime_error("Failed to export embedded glTF texture: " + outputPath.string());
        }
    }

    return outputPath.string();
}

std::string ResolveImagePath(
    const tinygltf::Model& model,
    const std::filesystem::path& modelPath,
    int textureIndex
)
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

    return ExportEmbeddedImage(modelPath, image, static_cast<size_t>(texture.source));
}

ModelMaterialData BuildMaterialData(
    const tinygltf::Model& model,
    const tinygltf::Material& material,
    const std::filesystem::path& modelPath
)
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
    materialData.metallicFactor = static_cast<float>(pbr.metallicFactor);
    materialData.roughnessFactor = static_cast<float>(pbr.roughnessFactor);

    const std::string metallicRoughnessTexturePath =
        ResolveImagePath(model, modelPath, pbr.metallicRoughnessTexture.index);
    materialData.metallicTexturePath = metallicRoughnessTexturePath;
    materialData.roughnessTexturePath = metallicRoughnessTexturePath;

    if (material.normalTexture.index >= 0)
    {
        materialData.normalTexturePath = ResolveImagePath(model, modelPath, material.normalTexture.index);
        materialData.normalScale = static_cast<float>(material.normalTexture.scale);
    }

    if (material.occlusionTexture.index >= 0)
    {
        materialData.occlusionTexturePath = ResolveImagePath(model, modelPath, material.occlusionTexture.index);
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
        materialData.emissiveIntensity = 1.0f;
    }

    materialData.opacity = material.alphaMode == "BLEND" || material.alphaMode == "MASK"
        ? materialData.baseColor[3]
        : 1.0f;
    materialData.baseColor[3] = materialData.opacity;
    if (material.alphaMode == "MASK")
    {
        materialData.alphaCutoff = static_cast<float>(material.alphaCutoff);
    }
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
                triangles.insert(triangles.end(), { a, b, c });
            }
            else
            {
                triangles.insert(triangles.end(), { b, a, c });
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

            triangles.insert(triangles.end(), { a, b, c });
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
    size_t primitiveIndex
)
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

void AppendPrimitive(
    const tinygltf::Model& model,
    const tinygltf::Node& node,
    const tinygltf::Mesh& mesh,
    const tinygltf::Primitive& primitive,
    size_t primitiveIndex,
    const glm::mat4& worldTransform,
    LoadedModelData& modelData
)
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
            1.0f
        );
        vertex.position[0] = position.x;
        vertex.position[1] = position.y;
        vertex.position[2] = position.z;
        ExpandBounds(glm::vec3(position), modelData);

        if (!texCoords.empty())
        {
            vertex.texCoord[0] = texCoords[vertexIndex * 2 + 0];
            vertex.texCoord[1] = texCoords[vertexIndex * 2 + 1];
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
                normals[vertexIndex * 3 + 2]
            ));
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
                tangents[vertexIndex * 4 + 2]
            ));
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
        primitive.mode >= 0 ? primitive.mode : kGltfModeTriangles
    );

    if (tangentHandednessScale < 0.0f)
    {
        // The node's world transform mirrors the geometry (negative determinant), which already
        // got baked into the vertex positions above. That mirroring reverses the apparent winding
        // of every triangle, so flip it back to CCW here to match the engine's backface-culling
        // convention (see VulkanPipeline's frontFace/cullMode setup).
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
    if (submeshData.mesh.IsValid())
    {
        modelData.submeshes.push_back(std::move(submeshData));
    }
}

void TraverseNode(
    const tinygltf::Model& model,
    int nodeIndex,
    const glm::mat4& parentTransform,
    LoadedModelData& modelData,
    std::unordered_set<int>& visitedNodes
)
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
                modelData
            );
        }
    }

    for (int childIndex : node.children)
    {
        TraverseNode(model, childIndex, worldTransform, modelData, visitedNodes);
    }
}

LoadedModelData BuildLoadedModelData(const tinygltf::Model& tinyModel, const std::filesystem::path& modelPath)
{
    LoadedModelData modelData{};
    modelData.materials.reserve(tinyModel.materials.size());
    for (const tinygltf::Material& material : tinyModel.materials)
    {
        modelData.materials.push_back(BuildMaterialData(tinyModel, material, modelPath));
    }

    std::unordered_set<int> visitedNodes;
    if (!tinyModel.scenes.empty())
    {
        const int sceneIndex = tinyModel.defaultScene >= 0 ? tinyModel.defaultScene : 0;
        EnsureIndexInRange(static_cast<size_t>(sceneIndex), tinyModel.scenes.size(), "scene");
        const tinygltf::Scene& scene = tinyModel.scenes[static_cast<size_t>(sceneIndex)];
        for (int rootNodeIndex : scene.nodes)
        {
            TraverseNode(tinyModel, rootNodeIndex, glm::mat4(1.0f), modelData, visitedNodes);
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

            TraverseNode(tinyModel, static_cast<int>(nodeIndex), glm::mat4(1.0f), modelData, visitedNodes);
            traversedAnyRoot = true;
        }

        if (!traversedAnyRoot)
        {
            for (size_t nodeIndex = 0; nodeIndex < tinyModel.nodes.size(); ++nodeIndex)
            {
                TraverseNode(tinyModel, static_cast<int>(nodeIndex), glm::mat4(1.0f), modelData, visitedNodes);
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

LoadedModelData GltfModelLoader::LoadModel(const std::string& path)
{
    const std::filesystem::path modelPath = std::filesystem::path(path).lexically_normal();
    const std::string extension = ToLowerCopy(modelPath.extension().string());

    tinygltf::TinyGLTF loader;
    loader.SetPreserveImageChannels(true);

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
            (errors.empty() ? std::string{} : ": " + errors)
        );
    }

    return BuildLoadedModelData(tinyModel, modelPath);
}
