#include "model_loader.h"
#include "fbx_model_loader.h"

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace
{
bool IsFbxModelPath(const std::string& path)
{
    std::filesystem::path modelPath(path);
    std::string extension = modelPath.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value)
    {
        return static_cast<char>(std::tolower(value));
    });
    return extension == ".fbx";
}

float Clamp01(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

std::string ResolveTexturePath(const std::filesystem::path& modelDirectory, const aiString& texturePath)
{
    const std::string rawPath = texturePath.C_Str();
    if (rawPath.empty() || rawPath[0] == '*')
    {
        return {};
    }

    std::filesystem::path resolvedPath(rawPath);
    if (!resolvedPath.is_absolute())
    {
        resolvedPath = modelDirectory / resolvedPath;
    }

    return resolvedPath.lexically_normal().string();
}

std::string ResolveMaterialTexturePath(
    const std::filesystem::path& modelDirectory,
    const aiMaterial* material,
    std::initializer_list<aiTextureType> textureTypes
)
{
    aiString texturePath;
    for (const aiTextureType textureType : textureTypes)
    {
        if (material->GetTexture(textureType, 0, &texturePath) == AI_SUCCESS)
        {
            return ResolveTexturePath(modelDirectory, texturePath);
        }
    }

    return {};
}

glm::vec3 ChooseOrthogonalTangent(const glm::vec3& normal)
{
    const glm::vec3 up = std::abs(normal.y) < 0.999f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 tangent = glm::cross(up, normal);
    if (glm::length(tangent) <= std::numeric_limits<float>::epsilon())
    {
        tangent = glm::vec3(1.0f, 0.0f, 0.0f);
    }
    else
    {
        tangent = glm::normalize(tangent);
    }

    return tangent;
}

void GenerateNormals(MeshData& meshData)
{
    for (Vertex& vertex : meshData.vertices)
    {
        vertex.normal[0] = 0.0f;
        vertex.normal[1] = 0.0f;
        vertex.normal[2] = 0.0f;
    }

    for (size_t index = 0; index + 2 < meshData.indices.size(); index += 3)
    {
        Vertex& vertex0 = meshData.vertices[meshData.indices[index + 0]];
        Vertex& vertex1 = meshData.vertices[meshData.indices[index + 1]];
        Vertex& vertex2 = meshData.vertices[meshData.indices[index + 2]];

        const glm::vec3 position0(vertex0.position[0], vertex0.position[1], vertex0.position[2]);
        const glm::vec3 position1(vertex1.position[0], vertex1.position[1], vertex1.position[2]);
        const glm::vec3 position2(vertex2.position[0], vertex2.position[1], vertex2.position[2]);

        const glm::vec3 edge01 = position1 - position0;
        const glm::vec3 edge02 = position2 - position0;
        const glm::vec3 faceNormal = glm::cross(edge01, edge02);
        if (glm::length(faceNormal) <= std::numeric_limits<float>::epsilon())
        {
            continue;
        }

        const glm::vec3 normalizedFaceNormal = glm::normalize(faceNormal);
        for (Vertex* vertex : { &vertex0, &vertex1, &vertex2 })
        {
            vertex->normal[0] += normalizedFaceNormal.x;
            vertex->normal[1] += normalizedFaceNormal.y;
            vertex->normal[2] += normalizedFaceNormal.z;
        }
    }

    for (Vertex& vertex : meshData.vertices)
    {
        glm::vec3 normal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
        if (glm::length(normal) <= std::numeric_limits<float>::epsilon())
        {
            normal = glm::vec3(0.0f, 1.0f, 0.0f);
        }
        else
        {
            normal = glm::normalize(normal);
        }

        vertex.normal[0] = normal.x;
        vertex.normal[1] = normal.y;
        vertex.normal[2] = normal.z;
    }
}

void GenerateTangents(MeshData& meshData, bool hasTexCoords)
{
    if (!hasTexCoords)
    {
        for (Vertex& vertex : meshData.vertices)
        {
            const glm::vec3 normal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
            const glm::vec3 tangent = ChooseOrthogonalTangent(normal);
            vertex.tangent[0] = tangent.x;
            vertex.tangent[1] = tangent.y;
            vertex.tangent[2] = tangent.z;
            vertex.tangent[3] = 1.0f;
        }
        return;
    }

    std::vector<glm::vec3> tangents(meshData.vertices.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> bitangents(meshData.vertices.size(), glm::vec3(0.0f));

    for (size_t index = 0; index + 2 < meshData.indices.size(); index += 3)
    {
        const uint32_t index0 = meshData.indices[index + 0];
        const uint32_t index1 = meshData.indices[index + 1];
        const uint32_t index2 = meshData.indices[index + 2];

        const Vertex& vertex0 = meshData.vertices[index0];
        const Vertex& vertex1 = meshData.vertices[index1];
        const Vertex& vertex2 = meshData.vertices[index2];

        const glm::vec3 position0(vertex0.position[0], vertex0.position[1], vertex0.position[2]);
        const glm::vec3 position1(vertex1.position[0], vertex1.position[1], vertex1.position[2]);
        const glm::vec3 position2(vertex2.position[0], vertex2.position[1], vertex2.position[2]);

        const glm::vec2 uv0(vertex0.texCoord[0], vertex0.texCoord[1]);
        const glm::vec2 uv1(vertex1.texCoord[0], vertex1.texCoord[1]);
        const glm::vec2 uv2(vertex2.texCoord[0], vertex2.texCoord[1]);

        const glm::vec3 edge01 = position1 - position0;
        const glm::vec3 edge02 = position2 - position0;
        const glm::vec2 deltaUv01 = uv1 - uv0;
        const glm::vec2 deltaUv02 = uv2 - uv0;

        const float determinant = deltaUv01.x * deltaUv02.y - deltaUv02.x * deltaUv01.y;
        if (std::abs(determinant) <= std::numeric_limits<float>::epsilon())
        {
            continue;
        }

        const float inverseDeterminant = 1.0f / determinant;
        const glm::vec3 triangleTangent =
            (edge01 * deltaUv02.y - edge02 * deltaUv01.y) * inverseDeterminant;
        const glm::vec3 triangleBitangent =
            (edge02 * deltaUv01.x - edge01 * deltaUv02.x) * inverseDeterminant;

        tangents[index0] += triangleTangent;
        tangents[index1] += triangleTangent;
        tangents[index2] += triangleTangent;
        bitangents[index0] += triangleBitangent;
        bitangents[index1] += triangleBitangent;
        bitangents[index2] += triangleBitangent;
    }

    for (size_t vertexIndex = 0; vertexIndex < meshData.vertices.size(); ++vertexIndex)
    {
        Vertex& vertex = meshData.vertices[vertexIndex];
        const glm::vec3 normal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);

        glm::vec3 tangent = tangents[vertexIndex];
        if (glm::length(tangent) <= std::numeric_limits<float>::epsilon())
        {
            tangent = ChooseOrthogonalTangent(normal);
        }
        else
        {
            tangent = glm::normalize(tangent - normal * glm::dot(normal, tangent));
        }

        glm::vec3 bitangent = bitangents[vertexIndex];
        if (glm::length(bitangent) <= std::numeric_limits<float>::epsilon())
        {
            bitangent = glm::normalize(glm::cross(normal, tangent));
        }
        else
        {
            bitangent = glm::normalize(bitangent);
        }

        const float handedness = glm::dot(glm::cross(normal, tangent), bitangent) < 0.0f ? -1.0f : 1.0f;
        vertex.tangent[0] = tangent.x;
        vertex.tangent[1] = tangent.y;
        vertex.tangent[2] = tangent.z;
        vertex.tangent[3] = handedness;
    }
}

ModelMaterialData LoadMaterialData(const std::filesystem::path& modelDirectory, const aiMaterial* material, uint32_t materialIndex)
{
    ModelMaterialData materialData{};

    aiString materialName;
    if (material->Get(AI_MATKEY_NAME, materialName) == AI_SUCCESS)
    {
        materialData.name = materialName.C_Str();
    }
    if (materialData.name.empty())
    {
        materialData.name = "Material " + std::to_string(materialIndex);
    }

    aiColor4D baseColor(1.0f, 1.0f, 1.0f, 1.0f);
    if (material->Get(AI_MATKEY_BASE_COLOR, baseColor) != AI_SUCCESS)
    {
        aiColor3D diffuseColor(1.0f, 1.0f, 1.0f);
        material->Get(AI_MATKEY_COLOR_DIFFUSE, diffuseColor);
        baseColor.r = diffuseColor.r;
        baseColor.g = diffuseColor.g;
        baseColor.b = diffuseColor.b;
        material->Get(AI_MATKEY_OPACITY, baseColor.a);
    }

    materialData.baseColor[0] = baseColor.r;
    materialData.baseColor[1] = baseColor.g;
    materialData.baseColor[2] = baseColor.b;
    materialData.baseColor[3] = baseColor.a;

    aiColor3D emissiveColor(0.0f, 0.0f, 0.0f);
    material->Get(AI_MATKEY_COLOR_EMISSIVE, emissiveColor);
    materialData.emissiveColor[0] = emissiveColor.r;
    materialData.emissiveColor[1] = emissiveColor.g;
    materialData.emissiveColor[2] = emissiveColor.b;

    float metallicFactor = materialData.metallicFactor;
    if (material->Get(AI_MATKEY_METALLIC_FACTOR, metallicFactor) == AI_SUCCESS)
    {
        materialData.metallicFactor = Clamp01(metallicFactor);
    }

    float roughnessFactor = materialData.roughnessFactor;
    if (material->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughnessFactor) == AI_SUCCESS)
    {
        materialData.roughnessFactor = Clamp01(roughnessFactor);
    }

    float normalScale = materialData.normalScale;
    if (material->Get(AI_MATKEY_BUMPSCALING, normalScale) == AI_SUCCESS)
    {
        materialData.normalScale = std::max(normalScale, 0.0f);
    }

    float emissiveIntensity = materialData.emissiveIntensity;
    if (material->Get(AI_MATKEY_EMISSIVE_INTENSITY, emissiveIntensity) == AI_SUCCESS)
    {
        materialData.emissiveIntensity = std::max(emissiveIntensity, 0.0f);
    }

    float opacity = materialData.opacity;
    if (material->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS)
    {
        materialData.opacity = Clamp01(opacity);
        materialData.baseColor[3] = materialData.opacity;
    }

    float shininess = 0.0f;
    if (material->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS && materialData.roughnessFactor >= 0.99f)
    {
        const float normalizedShininess = Clamp01(shininess / 128.0f);
        materialData.roughnessFactor = 1.0f - normalizedShininess;
    }

    materialData.baseColorTexturePath = ResolveMaterialTexturePath(
        modelDirectory,
        material,
        { aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE }
    );
    materialData.normalTexturePath = ResolveMaterialTexturePath(
        modelDirectory,
        material,
        { aiTextureType_NORMAL_CAMERA, aiTextureType_NORMALS }
    );
    materialData.metallicTexturePath = ResolveMaterialTexturePath(
        modelDirectory,
        material,
        { aiTextureType_METALNESS }
    );
    materialData.roughnessTexturePath = ResolveMaterialTexturePath(
        modelDirectory,
        material,
        { aiTextureType_DIFFUSE_ROUGHNESS }
    );
    materialData.occlusionTexturePath = ResolveMaterialTexturePath(
        modelDirectory,
        material,
        { aiTextureType_AMBIENT_OCCLUSION, aiTextureType_LIGHTMAP }
    );
    materialData.emissiveTexturePath = ResolveMaterialTexturePath(
        modelDirectory,
        material,
        { aiTextureType_EMISSION_COLOR, aiTextureType_EMISSIVE }
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
}

LoadedModelData LoadModelWithAssimp(const std::string& path)
{
    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_LINE | aiPrimitiveType_POINT);

    const aiScene* scene = importer.ReadFile(
        path,
        aiProcess_CalcTangentSpace |
        aiProcess_Triangulate |
        aiProcess_GenSmoothNormals |
        aiProcess_PreTransformVertices |
        aiProcess_FlipUVs |
        aiProcess_JoinIdenticalVertices |
        aiProcess_ImproveCacheLocality |
        aiProcess_FindInvalidData |
        aiProcess_SortByPType
    );

    if (scene == nullptr || scene->mRootNode == nullptr)
    {
        throw std::runtime_error("Failed to load model: " + path + " (" + importer.GetErrorString() + ")");
    }

    LoadedModelData modelData{};
    modelData.minBounds = glm::vec3(std::numeric_limits<float>::max());
    modelData.maxBounds = glm::vec3(std::numeric_limits<float>::lowest());

    const std::filesystem::path modelDirectory = std::filesystem::path(path).parent_path();
    modelData.materials.reserve(scene->mNumMaterials == 0 ? 1 : scene->mNumMaterials);

    if (scene->mNumMaterials == 0)
    {
        modelData.materials.push_back(ModelMaterialData{});
    }
    else
    {
        for (unsigned int materialIndex = 0; materialIndex < scene->mNumMaterials; ++materialIndex)
        {
            modelData.materials.push_back(LoadMaterialData(modelDirectory, scene->mMaterials[materialIndex], materialIndex));
        }
    }

    for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
    {
        const aiMesh* mesh = scene->mMeshes[meshIndex];
        const uint32_t materialIndex =
            mesh->mMaterialIndex < modelData.materials.size() ? mesh->mMaterialIndex : 0;
        const ModelMaterialData& materialData = modelData.materials[materialIndex];

        ModelSubmeshData submeshData{};
        submeshData.materialIndex = materialIndex;
        submeshData.hasTexCoords = mesh->HasTextureCoords(0);
        submeshData.hasNormals = mesh->HasNormals();
        submeshData.hasTangents = mesh->HasTangentsAndBitangents() && submeshData.hasTexCoords;
        submeshData.name = mesh->mName.length > 0 ? mesh->mName.C_Str() : ("Mesh " + std::to_string(meshIndex));
        submeshData.mesh.vertices.reserve(mesh->mNumVertices);
        submeshData.mesh.indices.reserve(mesh->mNumFaces * 3);

        for (unsigned int vertexIndex = 0; vertexIndex < mesh->mNumVertices; ++vertexIndex)
        {
            Vertex vertex{};
            vertex.position[0] = mesh->mVertices[vertexIndex].x;
            vertex.position[1] = mesh->mVertices[vertexIndex].y;
            vertex.position[2] = mesh->mVertices[vertexIndex].z;

            const glm::vec3 position(vertex.position[0], vertex.position[1], vertex.position[2]);
            if (!modelData.hasBounds)
            {
                modelData.minBounds = position;
                modelData.maxBounds = position;
                modelData.hasBounds = true;
            }
            else
            {
                modelData.minBounds = glm::min(modelData.minBounds, position);
                modelData.maxBounds = glm::max(modelData.maxBounds, position);
            }

            if (mesh->HasVertexColors(0))
            {
                vertex.color[0] = mesh->mColors[0][vertexIndex].r;
                vertex.color[1] = mesh->mColors[0][vertexIndex].g;
                vertex.color[2] = mesh->mColors[0][vertexIndex].b;
            }
            else
            {
                vertex.color[0] = 1.0f;
                vertex.color[1] = 1.0f;
                vertex.color[2] = 1.0f;
            }

            if (submeshData.hasTexCoords)
            {
                vertex.texCoord[0] = mesh->mTextureCoords[0][vertexIndex].x;
                vertex.texCoord[1] = mesh->mTextureCoords[0][vertexIndex].y;
            }
            else
            {
                vertex.texCoord[0] = 0.0f;
                vertex.texCoord[1] = 0.0f;
            }

            if (submeshData.hasNormals)
            {
                glm::vec3 normal(
                    mesh->mNormals[vertexIndex].x,
                    mesh->mNormals[vertexIndex].y,
                    mesh->mNormals[vertexIndex].z
                );
                if (glm::length(normal) > std::numeric_limits<float>::epsilon())
                {
                    normal = glm::normalize(normal);
                    vertex.normal[0] = normal.x;
                    vertex.normal[1] = normal.y;
                    vertex.normal[2] = normal.z;
                }
                else
                {
                    submeshData.hasNormals = false;
                    submeshData.hasTangents = false;
                    vertex.normal[0] = 0.0f;
                    vertex.normal[1] = 0.0f;
                    vertex.normal[2] = 0.0f;
                }
            }
            else
            {
                vertex.normal[0] = 0.0f;
                vertex.normal[1] = 0.0f;
                vertex.normal[2] = 0.0f;
            }

            if (submeshData.hasTangents)
            {
                glm::vec3 tangent(
                    mesh->mTangents[vertexIndex].x,
                    mesh->mTangents[vertexIndex].y,
                    mesh->mTangents[vertexIndex].z
                );
                glm::vec3 bitangent(
                    mesh->mBitangents[vertexIndex].x,
                    mesh->mBitangents[vertexIndex].y,
                    mesh->mBitangents[vertexIndex].z
                );

                if (glm::length(tangent) > std::numeric_limits<float>::epsilon() &&
                    glm::length(bitangent) > std::numeric_limits<float>::epsilon() &&
                    glm::length(glm::vec3(vertex.normal[0], vertex.normal[1], vertex.normal[2])) > std::numeric_limits<float>::epsilon())
                {
                    const glm::vec3 normal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
                    tangent = glm::normalize(tangent - normal * glm::dot(normal, tangent));
                    const float handedness = glm::dot(glm::cross(normal, tangent), bitangent) < 0.0f ? -1.0f : 1.0f;
                    vertex.tangent[0] = tangent.x;
                    vertex.tangent[1] = tangent.y;
                    vertex.tangent[2] = tangent.z;
                    vertex.tangent[3] = handedness;
                }
                else
                {
                    submeshData.hasTangents = false;
                    vertex.tangent[0] = 0.0f;
                    vertex.tangent[1] = 0.0f;
                    vertex.tangent[2] = 0.0f;
                    vertex.tangent[3] = 1.0f;
                }
            }
            else
            {
                vertex.tangent[0] = 0.0f;
                vertex.tangent[1] = 0.0f;
                vertex.tangent[2] = 0.0f;
                vertex.tangent[3] = 1.0f;
            }

            submeshData.mesh.vertices.push_back(vertex);
        }

        for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex)
        {
            const aiFace& face = mesh->mFaces[faceIndex];
            if (face.mNumIndices != 3)
            {
                continue;
            }

            submeshData.mesh.indices.push_back(face.mIndices[0]);
            submeshData.mesh.indices.push_back(face.mIndices[1]);
            submeshData.mesh.indices.push_back(face.mIndices[2]);
        }

        if (submeshData.mesh.IsValid())
        {
            if (!submeshData.hasNormals)
            {
                GenerateNormals(submeshData.mesh);
                submeshData.hasNormals = true;
            }
            if (!submeshData.hasTangents)
            {
                GenerateTangents(submeshData.mesh, submeshData.hasTexCoords);
                submeshData.hasTangents = true;
            }
            modelData.submeshes.push_back(std::move(submeshData));
        }
    }

    if (!modelData.IsValid())
    {
        throw std::runtime_error("Model did not contain renderable mesh data: " + path);
    }

    return modelData;
}

LoadedModelData ModelLoader::LoadModel(const std::string& path)
{
    if (IsFbxModelPath(path) && FbxModelLoader::IsAvailable())
    {
        return FbxModelLoader::LoadModel(path);
    }

    return LoadModelWithAssimp(path);
}
