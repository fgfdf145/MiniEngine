#pragma once

#include "material.h"
#include "mesh.h"

#include <scene_world.h>

#include <stdexcept>
#include <string>
#include <vector>

struct MaterialTexturePaths
{
    std::string baseColor;
    std::string normal;
    std::string metallic;
    std::string roughness;
    std::string occlusion;
    std::string emissive;
    std::string secondaryBaseColor;
    std::string secondaryNormal;
    std::string secondaryMetallic;
    std::string secondaryRoughness;
    std::string secondaryOcclusion;
    std::string secondaryEmissive;
    std::string blendMask;
};

struct CpuRenderSubmesh
{
    entt::entity entity = entt::null;
    MeshData mesh;
    MaterialPushConstants material;
    MaterialTexturePaths textures;
    bool hasTexCoords = false;
    std::string name;
};

class RendererWorld
{
public:
    void SetSceneWorld(ISceneWorld& sceneWorld);
    bool HasSceneWorld() const;
    ISceneWorld& GetSceneWorld();
    const ISceneWorld& GetSceneWorld() const;

    void SetRenderSubmeshes(std::vector<CpuRenderSubmesh> renderSubmeshes);
    void ClearRenderSubmeshes();
    const std::vector<CpuRenderSubmesh>& GetRenderSubmeshes() const;
    glm::mat4 GetModelMatrix(entt::entity entity) const;

private:
    ISceneWorld* m_sceneWorld = nullptr;
    std::vector<CpuRenderSubmesh> m_renderSubmeshes;
};
