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
    entry.model = std::move(model);
    entry.sourcePath = sourcePath;
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
        EvaluateJointPalette(skeleton, clip, entry.time, entry.palette);
        state.rendererWorld.SetJointPalette(entity, entry.palette);
        ++it;
    }
}
}
