#pragma once

#include "mesh.h"

#include <material_graph.h>
#include <glm/glm.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct ModelMaterialData
{
    std::string name;
    std::string baseColorTexturePath;
    std::string normalTexturePath;
    std::string metallicTexturePath;
    std::string roughnessTexturePath;
    std::string occlusionTexturePath;
    std::string emissiveTexturePath;
    float baseColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float emissiveColor[3] = { 0.0f, 0.0f, 0.0f };
    float metallicFactor = 0.0f;
    float roughnessFactor = 1.0f;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    float emissiveIntensity = 1.0f;
    float opacity = 1.0f;
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
    MaterialPbrSurfaceSettings pbr;
    MaterialTextureBlendGraph blendGraph;
    MaterialShaderGraph shaderGraph;
};

struct ModelSubmeshData
{
    MeshData mesh;
    uint32_t materialIndex = 0;
    bool hasTexCoords = false;
    bool hasNormals = false;
    bool hasTangents = false;
    std::string name;
};

struct LoadedModelData
{
    std::vector<ModelMaterialData> materials;
    std::vector<ModelSubmeshData> submeshes;
    glm::vec3 minBounds{ 0.0f, 0.0f, 0.0f };
    glm::vec3 maxBounds{ 0.0f, 0.0f, 0.0f };
    bool hasBounds = false;

    bool IsValid() const
    {
        return !submeshes.empty();
    }
};

// Invoked from the loading thread with the overall load fraction in [0, 1].
// Implementations must be cheap and thread-safe (typically an atomic store).
using ModelLoadProgressCallback = std::function<void(float)>;

class ModelLoader
{
public:
    static bool IsImportAvailable();
    static bool IsSupportedModelPath(const std::filesystem::path& path);
    static const char* GetImporterName();
    static LoadedModelData LoadModel(const std::string& path, const ModelLoadProgressCallback& progress = {});

    // Copies a model into targetDirectory. For an ASCII .gltf the referenced
    // companion files are copied too, sorted into subfolders (buffers/,
    // textures/) with the glTF's URIs rewritten to match; .glb is copied
    // as-is. Existing destination files are kept, never overwritten. Returns
    // the copied model's path.
    static std::filesystem::path CopyModelWithSortedReferences(
        const std::filesystem::path& modelPath,
        const std::filesystem::path& targetDirectory
    );
};
