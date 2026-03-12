#include "fbx_model_loader.h"

#if MINIENGINE_HAS_FBX_SDK

#include <fbxsdk.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <glm/geometric.hpp>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
float Clamp01(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

glm::vec3 ToVec3(const FbxDouble3& value)
{
    return glm::vec3(
        static_cast<float>(value[0]),
        static_cast<float>(value[1]),
        static_cast<float>(value[2])
    );
}

glm::vec4 ToVec4(const FbxDouble4& value)
{
    return glm::vec4(
        static_cast<float>(value[0]),
        static_cast<float>(value[1]),
        static_cast<float>(value[2]),
        static_cast<float>(value[3])
    );
}

glm::vec4 ToVec4(const FbxColor& value)
{
    return glm::vec4(
        static_cast<float>(value.mRed),
        static_cast<float>(value.mGreen),
        static_cast<float>(value.mBlue),
        static_cast<float>(value.mAlpha)
    );
}

glm::vec3 TransformPosition(const FbxAMatrix& transform, const FbxVector4& position)
{
    const FbxVector4 transformed = transform.MultT(position);
    return glm::vec3(
        static_cast<float>(transformed[0]),
        static_cast<float>(transformed[1]),
        static_cast<float>(transformed[2])
    );
}

glm::vec3 TransformDirection(const FbxAMatrix& transform, const FbxVector4& direction)
{
    const FbxVector4 transformed = transform.MultT(FbxVector4(direction[0], direction[1], direction[2], 0.0));
    glm::vec3 result(
        static_cast<float>(transformed[0]),
        static_cast<float>(transformed[1]),
        static_cast<float>(transformed[2])
    );

    if (glm::length(result) <= std::numeric_limits<float>::epsilon())
    {
        return glm::vec3(0.0f, 1.0f, 0.0f);
    }

    return glm::normalize(result);
}

FbxAMatrix GetGeometryTransform(const FbxNode* node)
{
    const FbxVector4 translation = node->GetGeometricTranslation(FbxNode::eSourcePivot);
    const FbxVector4 rotation = node->GetGeometricRotation(FbxNode::eSourcePivot);
    const FbxVector4 scaling = node->GetGeometricScaling(FbxNode::eSourcePivot);

    FbxAMatrix geometryTransform;
    geometryTransform.SetT(translation);
    geometryTransform.SetR(rotation);
    geometryTransform.SetS(scaling);
    return geometryTransform;
}

std::string ResolveTexturePath(
    const std::filesystem::path& modelDirectory,
    const char* fileName,
    const char* relativeFileName
)
{
    auto tryResolve = [&](const char* rawPath) -> std::string
    {
        if (rawPath == nullptr || rawPath[0] == '\0')
        {
            return {};
        }

        std::filesystem::path resolvedPath(rawPath);
        if (!resolvedPath.is_absolute())
        {
            resolvedPath = modelDirectory / resolvedPath;
        }

        return resolvedPath.lexically_normal().string();
    };

    if (const std::string resolved = tryResolve(fileName); !resolved.empty())
    {
        return resolved;
    }

    return tryResolve(relativeFileName);
}

std::string ExtractTexturePath(const std::filesystem::path& modelDirectory, const FbxProperty& property)
{
    if (!property.IsValid())
    {
        return {};
    }

    for (int layeredTextureIndex = 0; layeredTextureIndex < property.GetSrcObjectCount<FbxLayeredTexture>(); ++layeredTextureIndex)
    {
        const FbxLayeredTexture* layeredTexture = property.GetSrcObject<FbxLayeredTexture>(layeredTextureIndex);
        if (layeredTexture == nullptr)
        {
            continue;
        }

        for (int textureIndex = 0; textureIndex < layeredTexture->GetSrcObjectCount<FbxFileTexture>(); ++textureIndex)
        {
            const FbxFileTexture* texture = layeredTexture->GetSrcObject<FbxFileTexture>(textureIndex);
            if (texture == nullptr)
            {
                continue;
            }

            const std::string resolvedPath = ResolveTexturePath(
                modelDirectory,
                texture->GetFileName(),
                texture->GetRelativeFileName()
            );
            if (!resolvedPath.empty())
            {
                return resolvedPath;
            }
        }
    }

    for (int textureIndex = 0; textureIndex < property.GetSrcObjectCount<FbxFileTexture>(); ++textureIndex)
    {
        const FbxFileTexture* texture = property.GetSrcObject<FbxFileTexture>(textureIndex);
        if (texture == nullptr)
        {
            continue;
        }

        const std::string resolvedPath = ResolveTexturePath(
            modelDirectory,
            texture->GetFileName(),
            texture->GetRelativeFileName()
        );
        if (!resolvedPath.empty())
        {
            return resolvedPath;
        }
    }

    return {};
}

FbxProperty FindMaterialProperty(FbxSurfaceMaterial* material, std::initializer_list<const char*> propertyNames)
{
    for (const char* propertyName : propertyNames)
    {
        FbxProperty property = material->FindProperty(propertyName, false);
        if (property.IsValid())
        {
            return property;
        }
    }

    return {};
}

glm::vec3 ReadColorProperty(
    FbxSurfaceMaterial* material,
    std::initializer_list<const char*> colorPropertyNames,
    std::initializer_list<const char*> factorPropertyNames,
    const glm::vec3& fallback
)
{
    const FbxProperty colorProperty = FindMaterialProperty(material, colorPropertyNames);
    glm::vec3 color = fallback;
    if (colorProperty.IsValid())
    {
        color = ToVec3(colorProperty.Get<FbxDouble3>());
    }

    const FbxProperty factorProperty = FindMaterialProperty(material, factorPropertyNames);
    if (factorProperty.IsValid())
    {
        color *= static_cast<float>(factorProperty.Get<FbxDouble>());
    }

    return color;
}

float ReadScalarProperty(
    FbxSurfaceMaterial* material,
    std::initializer_list<const char*> propertyNames,
    float fallback
)
{
    const FbxProperty property = FindMaterialProperty(material, propertyNames);
    return property.IsValid() ? static_cast<float>(property.Get<FbxDouble>()) : fallback;
}

ModelMaterialData LoadMaterialData(
    const std::filesystem::path& modelDirectory,
    FbxSurfaceMaterial* material,
    uint32_t materialIndex
)
{
    ModelMaterialData materialData{};

    materialData.name = material != nullptr ? material->GetName() : "";
    if (materialData.name.empty())
    {
        materialData.name = "Material " + std::to_string(materialIndex);
    }

    if (material == nullptr)
    {
        return materialData;
    }

    const glm::vec3 baseColor = ReadColorProperty(
        material,
        { "Maya|baseColor", FbxSurfaceMaterial::sDiffuse },
        { "Maya|base", FbxSurfaceMaterial::sDiffuseFactor },
        glm::vec3(1.0f)
    );
    materialData.baseColor[0] = baseColor.x;
    materialData.baseColor[1] = baseColor.y;
    materialData.baseColor[2] = baseColor.z;

    const glm::vec3 emissiveColor = ReadColorProperty(
        material,
        { "Maya|emissionColor", FbxSurfaceMaterial::sEmissive },
        { "Maya|emission", FbxSurfaceMaterial::sEmissiveFactor },
        glm::vec3(0.0f)
    );
    materialData.emissiveColor[0] = emissiveColor.x;
    materialData.emissiveColor[1] = emissiveColor.y;
    materialData.emissiveColor[2] = emissiveColor.z;

    const float transparencyFactor = ReadScalarProperty(
        material,
        { "Maya|opacity", FbxSurfaceMaterial::sTransparencyFactor },
        0.0f
    );
    const FbxProperty transparentColorProperty = FindMaterialProperty(material, { FbxSurfaceMaterial::sTransparentColor });
    float transparentColorFactor = 0.0f;
    if (transparentColorProperty.IsValid())
    {
        const glm::vec3 transparentColor = ToVec3(transparentColorProperty.Get<FbxDouble3>());
        transparentColorFactor = Clamp01((transparentColor.x + transparentColor.y + transparentColor.z) / 3.0f);
    }

    materialData.opacity = Clamp01(1.0f - std::max(transparencyFactor, transparentColorFactor));
    materialData.baseColor[3] = materialData.opacity;

    materialData.metallicFactor = Clamp01(ReadScalarProperty(material, { "Maya|metalness" }, materialData.metallicFactor));
    materialData.roughnessFactor = Clamp01(ReadScalarProperty(
        material,
        { "Maya|roughness", "Maya|specularRoughness" },
        materialData.roughnessFactor
    ));
    materialData.normalScale = std::max(ReadScalarProperty(
        material,
        { "Maya|normalCamera", FbxSurfaceMaterial::sBumpFactor },
        materialData.normalScale
    ), 0.0f);
    materialData.emissiveIntensity = std::max(ReadScalarProperty(
        material,
        { "Maya|emission", FbxSurfaceMaterial::sEmissiveFactor },
        materialData.emissiveIntensity
    ), 0.0f);

    const float shininess = ReadScalarProperty(material, { FbxSurfaceMaterial::sShininess }, 0.0f);
    if (shininess > 0.0f && materialData.roughnessFactor >= 0.99f)
    {
        materialData.roughnessFactor = 1.0f - Clamp01(shininess / 128.0f);
    }

    materialData.baseColorTexturePath = ExtractTexturePath(
        modelDirectory,
        FindMaterialProperty(material, { "Maya|baseColor", FbxSurfaceMaterial::sDiffuse })
    );
    materialData.normalTexturePath = ExtractTexturePath(
        modelDirectory,
        FindMaterialProperty(material, { "Maya|normalCamera", FbxSurfaceMaterial::sNormalMap, FbxSurfaceMaterial::sBump })
    );
    materialData.metallicTexturePath = ExtractTexturePath(
        modelDirectory,
        FindMaterialProperty(material, { "Maya|metalness" })
    );
    materialData.roughnessTexturePath = ExtractTexturePath(
        modelDirectory,
        FindMaterialProperty(material, { "Maya|roughness", "Maya|specularRoughness" })
    );
    materialData.occlusionTexturePath = ExtractTexturePath(
        modelDirectory,
        FindMaterialProperty(material, { "Maya|AmbientOcclusion", "Maya|ao_map" })
    );
    materialData.emissiveTexturePath = ExtractTexturePath(
        modelDirectory,
        FindMaterialProperty(material, { "Maya|emissionColor", FbxSurfaceMaterial::sEmissive })
    );

    if (!materialData.emissiveTexturePath.empty() &&
        materialData.emissiveColor[0] == 0.0f &&
        materialData.emissiveColor[1] == 0.0f &&
        materialData.emissiveColor[2] == 0.0f)
    {
        materialData.emissiveColor[0] = 1.0f;
        materialData.emissiveColor[1] = 1.0f;
        materialData.emissiveColor[2] = 1.0f;
    }

    return materialData;
}

uint32_t ResolveSceneMaterialIndex(
    const std::unordered_map<const FbxSurfaceMaterial*, uint32_t>& materialLookup,
    const FbxSurfaceMaterial* material
)
{
    if (material == nullptr)
    {
        return 0;
    }

    if (const auto iterator = materialLookup.find(material); iterator != materialLookup.end())
    {
        return iterator->second;
    }

    return 0;
}

uint32_t ResolvePolygonMaterialIndex(
    const FbxMesh* mesh,
    const FbxNode* node,
    int polygonIndex,
    const std::unordered_map<const FbxSurfaceMaterial*, uint32_t>& materialLookup
)
{
    const FbxGeometryElementMaterial* materialElement = mesh->GetElementMaterial();
    if (materialElement == nullptr)
    {
        return ResolveSceneMaterialIndex(materialLookup, node->GetMaterial(0));
    }

    int materialSlotIndex = 0;
    switch (materialElement->GetMappingMode())
    {
    case FbxLayerElement::eAllSame:
        if (materialElement->GetIndexArray().GetCount() > 0)
        {
            materialSlotIndex = materialElement->GetIndexArray().GetAt(0);
        }
        break;
    case FbxLayerElement::eByPolygon:
        if (polygonIndex < materialElement->GetIndexArray().GetCount())
        {
            materialSlotIndex = materialElement->GetIndexArray().GetAt(polygonIndex);
        }
        break;
    default:
        materialSlotIndex = 0;
        break;
    }

    return ResolveSceneMaterialIndex(materialLookup, node->GetMaterial(materialSlotIndex));
}

bool TryReadVertexColor(
    const FbxMesh* mesh,
    int controlPointIndex,
    int polygonIndex,
    int polygonVertexIndex,
    glm::vec4& vertexColor
)
{
    const FbxGeometryElementVertexColor* colorElement = mesh->GetElementVertexColor();
    if (colorElement == nullptr)
    {
        return false;
    }

    int elementIndex = 0;
    switch (colorElement->GetMappingMode())
    {
    case FbxLayerElement::eByControlPoint:
        elementIndex = controlPointIndex;
        break;
    case FbxLayerElement::eByPolygonVertex:
        elementIndex = polygonIndex * 3 + polygonVertexIndex;
        break;
    case FbxLayerElement::eByPolygon:
        elementIndex = polygonIndex;
        break;
    case FbxLayerElement::eAllSame:
        elementIndex = 0;
        break;
    default:
        return false;
    }

    if (colorElement->GetReferenceMode() == FbxLayerElement::eIndexToDirect)
    {
        if (elementIndex < 0 || elementIndex >= colorElement->GetIndexArray().GetCount())
        {
            return false;
        }
        elementIndex = colorElement->GetIndexArray().GetAt(elementIndex);
    }

    if (elementIndex < 0 || elementIndex >= colorElement->GetDirectArray().GetCount())
    {
        return false;
    }

    vertexColor = ToVec4(colorElement->GetDirectArray().GetAt(elementIndex));
    return true;
}

struct VertexKey
{
    int controlPointIndex = 0;
    std::uint32_t colorX = 0;
    std::uint32_t colorY = 0;
    std::uint32_t colorZ = 0;
    std::uint32_t texCoordX = 0;
    std::uint32_t texCoordY = 0;
    std::uint32_t normalX = 0;
    std::uint32_t normalY = 0;
    std::uint32_t normalZ = 0;

    bool operator==(const VertexKey& other) const = default;
};

VertexKey BuildVertexKey(int controlPointIndex, const Vertex& vertex)
{
    return VertexKey{
        controlPointIndex,
        std::bit_cast<std::uint32_t>(vertex.color[0]),
        std::bit_cast<std::uint32_t>(vertex.color[1]),
        std::bit_cast<std::uint32_t>(vertex.color[2]),
        std::bit_cast<std::uint32_t>(vertex.texCoord[0]),
        std::bit_cast<std::uint32_t>(vertex.texCoord[1]),
        std::bit_cast<std::uint32_t>(vertex.normal[0]),
        std::bit_cast<std::uint32_t>(vertex.normal[1]),
        std::bit_cast<std::uint32_t>(vertex.normal[2])
    };
}

struct VertexKeyHash
{
    size_t operator()(const VertexKey& key) const
    {
        size_t hash = static_cast<size_t>(key.controlPointIndex);

        auto combine = [&hash](std::uint32_t value)
        {
            hash ^= static_cast<size_t>(value) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        };

        combine(key.colorX);
        combine(key.colorY);
        combine(key.colorZ);
        combine(key.texCoordX);
        combine(key.texCoordY);
        combine(key.normalX);
        combine(key.normalY);
        combine(key.normalZ);
        return hash;
    }
};

struct SubmeshBuildData
{
    ModelSubmeshData submesh;
    std::unordered_map<VertexKey, uint32_t, VertexKeyHash> vertexLookup;
};

void UpdateBounds(LoadedModelData& modelData, const glm::vec3& position)
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

void CollectNodeMeshes(
    FbxNode* node,
    LoadedModelData& modelData,
    const std::unordered_map<const FbxSurfaceMaterial*, uint32_t>& materialLookup
)
{
    if (node == nullptr)
    {
        return;
    }

    FbxMesh* mesh = node->GetMesh();
    if (mesh != nullptr)
    {
        if (mesh->GetPolygonCount() > 0 && mesh->GetElementNormalCount() == 0)
        {
            mesh->GenerateNormals(true, false, false);
        }

        FbxStringList uvSetNames;
        mesh->GetUVSetNames(uvSetNames);
        const bool hasTexCoords = uvSetNames.GetCount() > 0;
        const char* primaryUvSetName = hasTexCoords ? uvSetNames.GetStringAt(0) : nullptr;

        std::vector<SubmeshBuildData> submeshBuilds;
        std::unordered_map<uint32_t, size_t> submeshIndexByMaterial;

        const FbxAMatrix transform = node->EvaluateGlobalTransform() * GetGeometryTransform(node);
        const FbxAMatrix normalTransform = transform.Inverse().Transpose();
        const FbxVector4* controlPoints = mesh->GetControlPoints();
        const std::string meshName = mesh->GetName()[0] != '\0' ? mesh->GetName() : node->GetName();

        for (int polygonIndex = 0; polygonIndex < mesh->GetPolygonCount(); ++polygonIndex)
        {
            if (mesh->GetPolygonSize(polygonIndex) != 3)
            {
                continue;
            }

            const uint32_t materialIndex = ResolvePolygonMaterialIndex(mesh, node, polygonIndex, materialLookup);
            size_t submeshBuildIndex = 0;
            if (const auto iterator = submeshIndexByMaterial.find(materialIndex); iterator != submeshIndexByMaterial.end())
            {
                submeshBuildIndex = iterator->second;
            }
            else
            {
                submeshBuildIndex = submeshBuilds.size();
                submeshIndexByMaterial.emplace(materialIndex, submeshBuildIndex);

                SubmeshBuildData buildData{};
                buildData.submesh.materialIndex = materialIndex;
                buildData.submesh.hasTexCoords = false;
                buildData.submesh.hasNormals = false;
                buildData.submesh.hasTangents = false;
                buildData.submesh.name = meshName.empty()
                    ? ("Mesh " + std::to_string(modelData.submeshes.size() + submeshBuildIndex))
                    : meshName;

                if (node->GetMaterialCount() > 1 && materialIndex < modelData.materials.size())
                {
                    buildData.submesh.name += " [" + modelData.materials[materialIndex].name + "]";
                }

                submeshBuilds.push_back(std::move(buildData));
            }

            SubmeshBuildData& buildData = submeshBuilds[submeshBuildIndex];

            for (int polygonVertexIndex = 0; polygonVertexIndex < 3; ++polygonVertexIndex)
            {
                const int controlPointIndex = mesh->GetPolygonVertex(polygonIndex, polygonVertexIndex);
                const FbxVector4 controlPoint = controlPoints[controlPointIndex];
                const glm::vec3 transformedPosition = TransformPosition(transform, controlPoint);

                Vertex vertex{};
                vertex.position[0] = transformedPosition.x;
                vertex.position[1] = transformedPosition.y;
                vertex.position[2] = transformedPosition.z;
                UpdateBounds(modelData, transformedPosition);

                glm::vec4 vertexColor(1.0f);
                if (TryReadVertexColor(mesh, controlPointIndex, polygonIndex, polygonVertexIndex, vertexColor))
                {
                    vertex.color[0] = vertexColor.r;
                    vertex.color[1] = vertexColor.g;
                    vertex.color[2] = vertexColor.b;
                }
                else
                {
                    vertex.color[0] = 1.0f;
                    vertex.color[1] = 1.0f;
                    vertex.color[2] = 1.0f;
                }

                if (primaryUvSetName != nullptr)
                {
                    FbxVector2 uv;
                    bool unmappedUv = false;
                    if (mesh->GetPolygonVertexUV(polygonIndex, polygonVertexIndex, primaryUvSetName, uv, unmappedUv) && !unmappedUv)
                    {
                        vertex.texCoord[0] = static_cast<float>(uv[0]);
                        vertex.texCoord[1] = 1.0f - static_cast<float>(uv[1]);
                        buildData.submesh.hasTexCoords = true;
                    }
                    else
                    {
                        vertex.texCoord[0] = 0.0f;
                        vertex.texCoord[1] = 0.0f;
                    }
                }
                else
                {
                    vertex.texCoord[0] = 0.0f;
                    vertex.texCoord[1] = 0.0f;
                }

                FbxVector4 normal;
                if (mesh->GetPolygonVertexNormal(polygonIndex, polygonVertexIndex, normal))
                {
                    const glm::vec3 transformedNormal = TransformDirection(normalTransform, normal);
                    vertex.normal[0] = transformedNormal.x;
                    vertex.normal[1] = transformedNormal.y;
                    vertex.normal[2] = transformedNormal.z;
                    buildData.submesh.hasNormals = true;
                }
                else
                {
                    vertex.normal[0] = 0.0f;
                    vertex.normal[1] = 0.0f;
                    vertex.normal[2] = 0.0f;
                }

                vertex.tangent[0] = 0.0f;
                vertex.tangent[1] = 0.0f;
                vertex.tangent[2] = 0.0f;
                vertex.tangent[3] = 1.0f;

                const VertexKey key = BuildVertexKey(controlPointIndex, vertex);
                const auto iterator = buildData.vertexLookup.find(key);
                if (iterator != buildData.vertexLookup.end())
                {
                    buildData.submesh.mesh.indices.push_back(iterator->second);
                    continue;
                }

                const uint32_t vertexIndex = static_cast<uint32_t>(buildData.submesh.mesh.vertices.size());
                buildData.submesh.mesh.vertices.push_back(vertex);
                buildData.submesh.mesh.indices.push_back(vertexIndex);
                buildData.vertexLookup.emplace(key, vertexIndex);
            }
        }

        for (SubmeshBuildData& buildData : submeshBuilds)
        {
            if (buildData.submesh.mesh.IsValid())
            {
                modelData.submeshes.push_back(std::move(buildData.submesh));
            }
        }
    }

    for (int childIndex = 0; childIndex < node->GetChildCount(); ++childIndex)
    {
        CollectNodeMeshes(node->GetChild(childIndex), modelData, materialLookup);
    }
}
}

bool FbxModelLoader::IsAvailable()
{
    return true;
}

LoadedModelData FbxModelLoader::LoadModel(const std::string& path)
{
    FbxManager* manager = FbxManager::Create();
    if (manager == nullptr)
    {
        throw std::runtime_error("Failed to create FBX manager");
    }

    auto managerCleanup = std::unique_ptr<FbxManager, decltype([](FbxManager* instance)
    {
        if (instance != nullptr)
        {
            instance->Destroy();
        }
    })>(manager);

    FbxIOSettings* ioSettings = FbxIOSettings::Create(manager, IOSROOT);
    manager->SetIOSettings(ioSettings);

    FbxImporter* importer = FbxImporter::Create(manager, "");
    auto importerCleanup = std::unique_ptr<FbxImporter, decltype([](FbxImporter* instance)
    {
        if (instance != nullptr)
        {
            instance->Destroy();
        }
    })>(importer);

    if (!importer->Initialize(path.c_str(), -1, manager->GetIOSettings()))
    {
        throw std::runtime_error("Failed to initialize FBX importer for '" + path + "': " + importer->GetStatus().GetErrorString());
    }

    FbxScene* scene = FbxScene::Create(manager, "MiniEngineScene");
    if (scene == nullptr)
    {
        throw std::runtime_error("Failed to create FBX scene");
    }

    auto sceneCleanup = std::unique_ptr<FbxScene, decltype([](FbxScene* instance)
    {
        if (instance != nullptr)
        {
            instance->Destroy();
        }
    })>(scene);

    if (!importer->Import(scene))
    {
        throw std::runtime_error("Failed to import FBX scene from '" + path + "': " + importer->GetStatus().GetErrorString());
    }

    FbxGeometryConverter geometryConverter(manager);
    geometryConverter.Triangulate(scene, true, false);

    LoadedModelData modelData{};
    modelData.minBounds = glm::vec3(std::numeric_limits<float>::max());
    modelData.maxBounds = glm::vec3(std::numeric_limits<float>::lowest());

    const std::filesystem::path modelDirectory = std::filesystem::path(path).parent_path();
    modelData.materials.reserve(scene->GetMaterialCount() == 0 ? 1 : static_cast<size_t>(scene->GetMaterialCount()));

    std::unordered_map<const FbxSurfaceMaterial*, uint32_t> materialLookup;

    if (scene->GetMaterialCount() == 0)
    {
        modelData.materials.push_back(ModelMaterialData{});
    }
    else
    {
        for (int materialIndex = 0; materialIndex < scene->GetMaterialCount(); ++materialIndex)
        {
            FbxSurfaceMaterial* material = scene->GetMaterial(materialIndex);
            const uint32_t outputMaterialIndex = static_cast<uint32_t>(modelData.materials.size());
            materialLookup.emplace(material, outputMaterialIndex);
            modelData.materials.push_back(LoadMaterialData(modelDirectory, material, outputMaterialIndex));
        }
    }

    CollectNodeMeshes(scene->GetRootNode(), modelData, materialLookup);

    if (!modelData.IsValid())
    {
        throw std::runtime_error("FBX scene did not contain renderable mesh data: " + path);
    }

    return modelData;
}

#else

bool FbxModelLoader::IsAvailable()
{
    return false;
}

LoadedModelData FbxModelLoader::LoadModel(const std::string& path)
{
    throw std::runtime_error("FBX SDK support is not available in this build: " + path);
}

#endif
