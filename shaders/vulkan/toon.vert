#version 450
#extension GL_GOOGLE_include_directive : require

// The toon passes' vertex stage (toon_pass.h): an anime character's surface, or under kOutline its
// outline, the hull pushed out along the smoothed normal (Anime Outline's vertex stage).
layout(constant_id = 0) const bool kOutline = false;

#define TOON_VERTEX_STAGE
#include "toon_common.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec3 inNormal;
layout(location = 5) in vec2 inTexCoord1;
layout(location = 6) in vec3 inOutlineNormal;
// Where this vertex was last frame (VulkanBuffer::GetPreviousPositionHandle), and the draw's model
// matrix then (by its firstInstance, as triangle.vert reads it): the motion the toon pass writes.
layout(location = 7) in vec3 inPreviousPosition;
layout(set = 0, binding = 2) readonly buffer PreviousModelBuffer
{
    mat4 previousModels[];
}
previousModelData;

layout(location = 0) out vec3 fragWorldPosition;
// The shading normal, which the face normal fix bends toward the face's forward.
layout(location = 1) out vec3 fragWorldNormal;
layout(location = 2) out vec2 fragTexCoord;
layout(location = 3) out vec2 fragTexCoord1;
// The smoothed normal, which the contact shadow may follow (_DepthTexShadowIgnoreLightDir).
layout(location = 4) out vec3 fragSmoothNormal;
// The vertex normal unbent, which the outline is coloured by.
layout(location = 5) out vec3 fragGeometryNormal;
layout(location = 6) flat out uint fragToonIndex;
// Unjittered clip positions now and last frame, for the velocity (as triangle.vert's).
layout(location = 7) out vec4 fragCurrClip;
layout(location = 8) out vec4 fragPrevClip;

// The same expression as triangle.vert's, so an opaque surface lands on the depth the geometry pass
// wrote for it.
invariant gl_Position;

void main()
{
    ToonMaterial material = toonMaterials.materials[drawData.toonIndex];
    vec4 worldPosition = drawData.model * vec4(inPosition, 1.0);

    // As triangle.vert: the normal matrix of a TRS model is each column over its squared length.
    vec3 mc0 = drawData.model[0].xyz;
    vec3 mc1 = drawData.model[1].xyz;
    vec3 mc2 = drawData.model[2].xyz;
    mat3 normalMatrix = mat3(
        mc0 / max(dot(mc0, mc0), 1e-6),
        mc1 / max(dot(mc1, mc1), 1e-6),
        mc2 / max(dot(mc2, mc2), 1e-6));
    vec3 normal = normalize(normalMatrix * inNormal);
    vec3 smoothNormal = dot(inOutlineNormal, inOutlineNormal) > 0.25 ? normalize(normalMatrix * inOutlineNormal) : normal;
    float faceMask = ToonFaceMask(material, inTexCoord);

    // The face normal fix: on the face, the normal leans toward the face's forward (flatten) or out
    // from a sphere around the head, by the fix's method, so the face's light reads as one plane.
    vec3 shadingNormal = normal;
    float fixAmount = material.faceSdf2.y * faceMask;
    if (fixAmount > 0.0)
    {
        vec3 forward = ToonFaceForward(material);
        vec3 fromHead = worldPosition.xyz - ToonHeadPosition(material);
        vec3 target = forward + material.faceSdf2.z * (fromHead - forward);
        shadingNormal = normalize(mix(normal, target, fixAmount));
    }

    if (kOutline)
    {
        // The width grows with distance and field of view up to a cap (Unity's 60 for the product
        // of the distance and the vertical field of view in degrees), so the line keeps its width on
        // screen near the camera and thins far away.
        float distance = -(ubo.view * worldPosition).z;
        float fovDegrees = degrees(2.0 * atan(1.0 / max(abs(ubo.proj[1][1]), 1e-6)));
        float scale = min(abs(distance) * fovDegrees, 60.0);
        float width = material.outline.x;
        if (ToonHas(material, TOON_FEATURE_OUTLINE_WIDTH_TEXTURE))
        {
            width *= textureLod(toonOutlineWidthMap, inTexCoord, 0.0).g;
        }
        worldPosition.xyz += smoothNormal * (width * scale * 0.00005);

        // Pushed away from the camera by the outline's z offset (the face's own on the face), its
        // depth alone, so it hides behind the surface it outlines without moving on screen.
        vec4 viewPosition = ubo.view * worldPosition;
        vec4 clip = ubo.proj * viewPosition;
        float zOffset = mix(material.outline.y, material.outline.z, faceMask) * max(scale * 0.02, 1.0) + material.depthTexRim2.z;
        vec4 pushed = ubo.proj * vec4(viewPosition.xy, viewPosition.z - zOffset, 1.0);
        gl_Position = vec4(clip.xy, pushed.z / pushed.w * clip.w, clip.w);
    }
    else
    {
        gl_Position = ubo.proj * ubo.view * worldPosition;
    }

    fragWorldPosition = worldPosition.xyz;
    fragWorldNormal = shadingNormal;
    fragGeometryNormal = normal;
    fragSmoothNormal = smoothNormal;
    fragTexCoord = inTexCoord;
    fragTexCoord1 = inTexCoord1;
    fragToonIndex = drawData.toonIndex;
    fragCurrClip = ubo.viewProjNoJitter * worldPosition;
    fragPrevClip = ubo.prevViewProj * (previousModelData.previousModels[gl_InstanceIndex] * vec4(inPreviousPosition, 1.0));
}
