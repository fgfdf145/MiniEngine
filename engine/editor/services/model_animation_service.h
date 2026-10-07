#pragma once

#include <engine/asset/model_driver_pose.h>
#include <engine/asset/model_loader.h>

#include <entt/entt.hpp>

#include <memory>
#include <optional>
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

    // Poses the entity as a seated driver (PoseDriver) instead of playing its clip, from the next Tick
    // until cleared. Ignored for an entity not tracked, or whose skeleton is not a humanoid's.
    void SetDriverPose(entt::entity entity, const DriverPoseInput& input);
    void ClearDriverPose(entt::entity entity);
    // What the last Tick's driver pose gave; null for an entity not posed as a driver.
    const DriverPoseResult* GetDriverPoseResult(entt::entity entity) const;
    // The humanoid rig of a tracked entity's skeleton, null when it has none.
    const DriverRig* GetDriverRig(entt::entity entity) const;

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
        // Found once per model: the skeleton's humanoid joints, if it has them.
        std::optional<DriverRig> rig;
        bool rigSearched = false;
        std::optional<DriverPoseInput> driver;
        std::optional<DriverPoseResult> driverResult;
    };
    std::unordered_map<entt::entity, Entry> m_entries;
};
}
