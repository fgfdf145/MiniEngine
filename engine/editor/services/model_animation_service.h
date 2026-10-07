#pragma once

#include <engine/asset/model_loader.h>

#include <entt/entt.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace me
{

struct RendererSharedState;

// Plays the glTF animations of the scene's skinned models (ModelSkeleton): every frame, each tracked
// entity's clip (ModelComponent::animationClip and the rest) advances and its joint palette goes to the
// renderer world, which the skinning pass deforms the entity's skinned submeshes by.
class ModelAnimationPlayback
{
  public:
    // Called when an entity's renderables are built from a model with skins: plays it from now on.
    // The same model again keeps the entity's time.
    void Track(entt::entity entity, std::shared_ptr<const LoadedModelData> model, const std::string& sourcePath);
    // The entity's clip starts again from its beginning.
    void Restart(entt::entity entity);
    void Forget(entt::entity entity);
    void Clear();

    // Advances every tracked entity's clip by deltaSeconds (times its speed, while it plays) and sets
    // its joint palette; drops the entities the scene no longer has, or that hold another model now.
    void Tick(RendererSharedState& state, float deltaSeconds);

    // The clip ModelComponent::animationClip names for this model: by name, or for an empty name one
    // called "idle" (any case), else the first; -1 for none.
    static int32_t ResolveClip(const ModelSkeleton& skeleton, const std::string& clip);

  private:
    struct Entry
    {
        std::shared_ptr<const LoadedModelData> model;
        std::string sourcePath;
        float time = 0.0f;
        std::vector<glm::mat4> palette;
    };
    std::unordered_map<entt::entity, Entry> m_entries;
};
}
