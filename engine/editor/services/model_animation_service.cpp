#include "model_animation_service.h"

#include <engine/core/text/ascii.h>
#include <engine/editor/renderer_shared_state.h>

#include <cmath>

namespace me
{

void ModelAnimationPlayback::Track(entt::entity entity, std::shared_ptr<const LoadedModelData> model, const std::string& sourcePath)
{
    Entry& entry = m_entries[entity];
    if (entry.sourcePath != sourcePath)
    {
        entry.time = 0.0f;
    }
    if (entry.model != model)
    {
        entry.rig.reset();
        entry.rigSearched = false;
    }
    entry.model = std::move(model);
    entry.sourcePath = sourcePath;
}

void ModelAnimationPlayback::SetDriverPose(entt::entity entity, const DriverPoseInput& input)
{
    if (const auto found = m_entries.find(entity); found != m_entries.end())
    {
        found->second.driver = input;
    }
}

void ModelAnimationPlayback::ClearDriverPose(entt::entity entity)
{
    if (const auto found = m_entries.find(entity); found != m_entries.end())
    {
        found->second.driver.reset();
        found->second.driverResult.reset();
    }
}

const DriverPoseResult* ModelAnimationPlayback::GetDriverPoseResult(entt::entity entity) const
{
    const auto found = m_entries.find(entity);
    return found != m_entries.end() && found->second.driverResult.has_value() ? &*found->second.driverResult : nullptr;
}

const DriverRig* ModelAnimationPlayback::GetDriverRig(entt::entity entity) const
{
    const auto found = m_entries.find(entity);
    return found != m_entries.end() && found->second.rig.has_value() ? &*found->second.rig : nullptr;
}

void ModelAnimationPlayback::Restart(entt::entity entity)
{
    if (const auto found = m_entries.find(entity); found != m_entries.end())
    {
        found->second.time = 0.0f;
    }
}

void ModelAnimationPlayback::Forget(entt::entity entity)
{
    m_entries.erase(entity);
}

void ModelAnimationPlayback::Clear()
{
    m_entries.clear();
}

int32_t ModelAnimationPlayback::ResolveClip(const ModelSkeleton& skeleton, const std::string& clip)
{
    if (!clip.empty())
    {
        return skeleton.FindClip(clip);
    }
    for (size_t index = 0; index < skeleton.clips.size(); ++index)
    {
        if (ToLowerAscii(skeleton.clips[index].name) == "idle")
        {
            return static_cast<int32_t>(index);
        }
    }
    return skeleton.clips.empty() ? -1 : 0;
}

void ModelAnimationPlayback::Tick(RendererSharedState& state, float deltaSeconds)
{
    if (m_entries.empty() || !state.editorWorld)
    {
        return;
    }
    IEditorWorld& world = state.GetEditorWorld();
    for (auto it = m_entries.begin(); it != m_entries.end();)
    {
        const entt::entity entity = it->first;
        Entry& entry = it->second;
        const bool valid = world.HasModelComponent(entity) && world.GetModel(entity).sourcePath == entry.sourcePath && entry.model &&
                           entry.model->skeleton;
        if (!valid)
        {
            state.rendererWorld.ClearJointPalette(entity);
            it = m_entries.erase(it);
            continue;
        }
        const ModelComponent& model = world.GetModel(entity);
        const ModelSkeleton& skeleton = *entry.model->skeleton;
        if (!entry.rigSearched)
        {
            entry.rig = FindDriverRig(skeleton);
            entry.springBones = BuildSpringBones(skeleton);
            entry.springState = SpringBoneState{};
            entry.rigSearched = true;
        }
        const glm::mat4 modelToWorld = world.GetModelMatrix(entity);
        if (entry.driver.has_value() && entry.rig.has_value())
        {
            // Posed without hiding the head, so that the hair swings from it, then hidden.
            DriverPoseInput input = *entry.driver;
            input.hideHead = false;
            DriverPoseResult result;
            PoseDriver(skeleton, *entry.rig, input, entry.poses, &result);
            if (model.springBones)
            {
                SimulateSpringBones(skeleton, entry.springBones, entry.springState, entry.poses, modelToWorld, deltaSeconds, input.seatPlanes);
            }
            else
            {
                entry.springState = SpringBoneState{};
            }
            if (entry.driver->hideHead)
            {
                HideDriverHead(*entry.rig, entry.poses);
            }
            ComputeNodeWorldMatrices(skeleton, entry.poses, entry.world);
            PaletteFromNodeWorldMatrices(skeleton, entry.world, entry.palette);
            entry.driverResult = result;
            state.rendererWorld.SetJointPalette(entity, entry.palette);
            ++it;
            continue;
        }
        entry.driverResult.reset();
        const int32_t clip = model.animationEnabled ? ResolveClip(skeleton, model.animationClip) : -1;
        if (clip >= 0 && model.animationPlaying)
        {
            entry.time += deltaSeconds * model.animationSpeed;
            const float duration = skeleton.clips[static_cast<size_t>(clip)].duration;
            if (duration > 0.0f && (entry.time > duration * 64.0f || entry.time < -duration * 64.0f))
            {
                // Kept near the clip's range, where a float still resolves a frame.
                entry.time = std::fmod(entry.time, duration);
            }
        }
        if (clip >= 0)
        {
            // The clip, and the hair and skirt swinging from it.
            EvaluateNodePoses(skeleton, clip, entry.time, entry.poses);
            if (model.springBones)
            {
                SimulateSpringBones(skeleton, entry.springBones, entry.springState, entry.poses, modelToWorld, deltaSeconds);
            }
            else
            {
                entry.springState = SpringBoneState{};
            }
            ComputeNodeWorldMatrices(skeleton, entry.poses, entry.world);
            PaletteFromNodeWorldMatrices(skeleton, entry.world, entry.palette);
        }
        else
        {
            // The bind pose, which the nodes' rest transforms need not be.
            EvaluateJointPalette(skeleton, -1, 0.0f, entry.palette);
            entry.springState = SpringBoneState{};
        }
        state.rendererWorld.SetJointPalette(entity, entry.palette);
        ++it;
    }
}
}
