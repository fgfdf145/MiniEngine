#version 450
#extension GL_GOOGLE_include_directive : require

#include "scene_common.glsl"

// ObjectPushConstants in engine/renderer/material.h. The material is in set 0 binding 12.
layout(push_constant) uniform DrawConstants
{
    mat4 model;
}
drawData;

// Each draw's model matrix from last frame, indexed by the draw's firstInstance (see
// VulkanDrawItem::motionSlot). Push constants are full, so it cannot ride with the current one.
layout(set = 0, binding = 2) readonly buffer PreviousModelBuffer
{
    mat4 previousModels[];
}
previousModelData;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec3 inNormal;
layout(location = 4) in vec4 inTangent;
layout(location = 5) in vec2 inTexCoord1;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out vec3 fragWorldNormal;
layout(location = 3) out vec4 fragWorldTangent;
layout(location = 4) out vec3 fragWorldPosition;
// Clip positions of this vertex this frame and last frame, divided per fragment by gbuffer.frag
// for motion vectors. triangle.frag does not declare them.
layout(location = 5) out vec4 fragCurrClip;
layout(location = 6) out vec4 fragPrevClip;
// The draw's slot (its firstInstance), which the fragment shaders index the material buffer with.
layout(location = 7) flat out uint fragDrawSlot;
layout(location = 8) out vec2 fragTexCoord1;

void main()
{
    vec4 worldPosition = drawData.model * vec4(inPosition, 1.0);

    // Compute the normal matrix analytically from the TRS model columns.
    // For M = R*S, the normal matrix (transpose of inverse) equals R*S^{-1},
    // which is each column divided by its squared length. This avoids inverse().
    vec3 mc0 = drawData.model[0].xyz;
    vec3 mc1 = drawData.model[1].xyz;
    vec3 mc2 = drawData.model[2].xyz;
    mat3 normalMatrix = mat3(
        mc0 / max(dot(mc0, mc0), 1e-6),
        mc1 / max(dot(mc1, mc1), 1e-6),
        mc2 / max(dot(mc2, mc2), 1e-6));

    vec3 worldTangent = normalize(mat3(drawData.model) * inTangent.xyz);

    gl_Position = ubo.proj * ubo.view * worldPosition;
    // Unjittered, like prevViewProj, so motion vectors measure motion and not the TAA jitter.
    fragCurrClip = ubo.viewProjNoJitter * worldPosition;
    fragPrevClip = ubo.prevViewProj * (previousModelData.previousModels[gl_InstanceIndex] * vec4(inPosition, 1.0));
    fragColor = inColor;
    fragTexCoord = inTexCoord;
    fragTexCoord1 = inTexCoord1;
    fragWorldNormal = normalize(normalMatrix * inNormal);
    fragWorldTangent = vec4(worldTangent, inTangent.w);
    fragWorldPosition = worldPosition.xyz;
    fragDrawSlot = uint(gl_InstanceIndex);
}
