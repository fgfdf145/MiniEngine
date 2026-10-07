#include "model_animation.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace me
{

namespace
{
glm::quat ToQuat(const glm::vec4& value)
{
    return glm::quat(value.w, value.x, value.y, value.z);
}

// The channel's value at time (inside its keys' range, clamped at either end), as glTF interpolates
// it: rotations along the shorter arc.
glm::vec4 SampleChannel(const ModelAnimationChannel& channel, float time)
{
    const std::vector<float>& times = channel.times;
    const bool cubic = channel.interpolation == ModelAnimationInterpolation::CubicSpline;
    const auto value = [&](size_t key)
    {
        return channel.values[cubic ? key * 3 + 1 : key];
    };
    if (times.size() == 1 || time <= times.front())
    {
        return value(0);
    }
    if (time >= times.back())
    {
        return value(times.size() - 1);
    }
    const size_t next = static_cast<size_t>(std::upper_bound(times.begin(), times.end(), time) - times.begin());
    const size_t key = next - 1;
    const float span = times[next] - times[key];
    const float t = span > 0.0f ? (time - times[key]) / span : 0.0f;
    switch (channel.interpolation)
    {
    case ModelAnimationInterpolation::Step:
        return value(key);
    case ModelAnimationInterpolation::CubicSpline:
    {
        const glm::vec4 p0 = value(key);
        const glm::vec4 m0 = channel.values[key * 3 + 2] * span;
        const glm::vec4 p1 = value(next);
        const glm::vec4 m1 = channel.values[next * 3] * span;
        const float t2 = t * t;
        const float t3 = t2 * t;
        glm::vec4 result = (2.0f * t3 - 3.0f * t2 + 1.0f) * p0 + (t3 - 2.0f * t2 + t) * m0 + (-2.0f * t3 + 3.0f * t2) * p1 + (t3 - t2) * m1;
        if (channel.path == ModelAnimationPath::Rotation)
        {
            result = glm::normalize(result);
        }
        return result;
    }
    case ModelAnimationInterpolation::Linear:
    default:
        if (channel.path == ModelAnimationPath::Rotation)
        {
            const glm::quat rotation = glm::slerp(ToQuat(value(key)), ToQuat(value(next)), t);
            return glm::vec4(rotation.x, rotation.y, rotation.z, rotation.w);
        }
        return glm::mix(value(key), value(next), t);
    }
}
}

int32_t ModelSkeleton::FindClip(const std::string& name) const
{
    for (size_t index = 0; index < clips.size(); ++index)
    {
        if (clips[index].name == name)
        {
            return static_cast<int32_t>(index);
        }
    }
    return -1;
}

int32_t ModelSkeleton::FindNode(const std::string& name) const
{
    for (size_t index = 0; index < nodes.size(); ++index)
    {
        if (nodes[index].name == name)
        {
            return static_cast<int32_t>(index);
        }
    }
    return -1;
}

void RestNodePoses(const ModelSkeleton& skeleton, std::vector<ModelNodePose>& poses)
{
    poses.resize(skeleton.nodes.size());
    for (size_t index = 0; index < skeleton.nodes.size(); ++index)
    {
        const ModelSkeletonNode& node = skeleton.nodes[index];
        poses[index] = ModelNodePose{node.translation, node.rotation, node.scale, false};
    }
}

void EvaluateNodePoses(const ModelSkeleton& skeleton, int32_t clip, float time, std::vector<ModelNodePose>& poses)
{
    RestNodePoses(skeleton, poses);
    if (clip < 0 || static_cast<size_t>(clip) >= skeleton.clips.size())
    {
        return;
    }
    const ModelAnimationClip& animation = skeleton.clips[static_cast<size_t>(clip)];
    if (animation.duration > 0.0f)
    {
        time = std::fmod(time, animation.duration);
        if (time < 0.0f)
        {
            time += animation.duration;
        }
    }
    for (const ModelAnimationChannel& channel : animation.channels)
    {
        if (channel.node < 0 || static_cast<size_t>(channel.node) >= poses.size() || channel.times.empty())
        {
            continue;
        }
        ModelNodePose& pose = poses[static_cast<size_t>(channel.node)];
        const glm::vec4 value = SampleChannel(channel, time);
        switch (channel.path)
        {
        case ModelAnimationPath::Translation:
            pose.translation = glm::vec3(value);
            break;
        case ModelAnimationPath::Rotation:
            pose.rotation = glm::normalize(ToQuat(value));
            break;
        case ModelAnimationPath::Scale:
            pose.scale = glm::vec3(value);
            break;
        }
        pose.posed = true;
    }
}

void ComputeNodeWorldMatrices(const ModelSkeleton& skeleton, const std::vector<ModelNodePose>& poses, std::vector<glm::mat4>& world)
{
    world.assign(skeleton.nodes.size(), glm::mat4(1.0f));
    for (const int32_t index : skeleton.order)
    {
        const ModelSkeletonNode& node = skeleton.nodes[static_cast<size_t>(index)];
        const ModelNodePose& pose = poses[static_cast<size_t>(index)];
        // A node given as a matrix keeps it unless an animation moves it, which then replaces it
        // (glTF forbids animating such a node; the decomposed rest pose stands in).
        glm::mat4 local = node.matrix;
        if (!node.hasMatrix || pose.posed)
        {
            local = glm::translate(glm::mat4(1.0f), pose.translation) * glm::mat4_cast(pose.rotation) *
                    glm::scale(glm::mat4(1.0f), pose.scale);
        }
        world[static_cast<size_t>(index)] = node.parent >= 0 ? world[static_cast<size_t>(node.parent)] * local : local;
    }
}

void PaletteFromNodeWorldMatrices(const ModelSkeleton& skeleton, const std::vector<glm::mat4>& world, std::vector<glm::mat4>& palette)
{
    palette.assign(skeleton.paletteSize, glm::mat4(1.0f));
    for (const ModelSkinBinding& binding : skeleton.bindings)
    {
        for (size_t joint = 0; joint < binding.jointNodes.size(); ++joint)
        {
            const int32_t node = binding.jointNodes[joint];
            if (node < 0 || static_cast<size_t>(node) >= world.size())
            {
                continue;
            }
            palette[binding.paletteOffset + joint] =
                world[static_cast<size_t>(node)] * binding.inverseBindMatrices[joint] * binding.bakedInverse;
        }
    }
}

void EvaluateJointPalette(const ModelSkeleton& skeleton, int32_t clip, float time, std::vector<glm::mat4>& palette)
{
    if (clip < 0 || static_cast<size_t>(clip) >= skeleton.clips.size())
    {
        palette.assign(skeleton.paletteSize, glm::mat4(1.0f));
        return;
    }
    std::vector<ModelNodePose> poses;
    EvaluateNodePoses(skeleton, clip, time, poses);
    std::vector<glm::mat4> world;
    ComputeNodeWorldMatrices(skeleton, poses, world);
    PaletteFromNodeWorldMatrices(skeleton, world, palette);
}
}
