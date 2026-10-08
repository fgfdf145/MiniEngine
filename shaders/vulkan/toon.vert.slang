// The toon passes' vertex stage (toon_pass.h): an anime character's surface, or under kOutline its
// outline, the hull pushed out along the smoothed normal (Anime Outline's vertex stage).
[[vk::constant_id(0)]] const bool kOutline = false;

#define TOON_VERTEX_STAGE
#include "shader_helpers.slang"
#include "toon_common.slang"

[[vk::binding(2, 0)]] StructuredBuffer<float4x4> previousModels;

struct VertexInput
{
    [[vk::location(0)]] float3 inPosition;
    [[vk::location(2)]] float2 inTexCoord;
    [[vk::location(3)]] float3 inNormal;
    [[vk::location(5)]] float2 inTexCoord1;
    [[vk::location(6)]] float3 inOutlineNormal;
    // Where this vertex was last frame (VulkanBuffer::GetPreviousPositionHandle), and the draw's model
    // matrix then (by its firstInstance, as triangle.vert reads it): the motion the toon pass writes.
    [[vk::location(7)]] float3 inPreviousPosition;
    uint instanceIndex : SV_VulkanInstanceID;
};

struct VertexOutput
{
    // The same expression as triangle.vert's, so an opaque surface lands on the depth the geometry
    // pass wrote for it; invariant like it (MINIENGINE_INVARIANT_POSITION_SHADERS).
    float4 position : SV_Position;
    [[vk::location(0)]] float3 fragWorldPosition;
    // The shading normal, which the face normal fix bends toward the face's forward.
    [[vk::location(1)]] float3 fragWorldNormal;
    [[vk::location(2)]] float2 fragTexCoord;
    [[vk::location(3)]] float2 fragTexCoord1;
    // The smoothed normal, which the contact shadow may follow (_DepthTexShadowIgnoreLightDir).
    [[vk::location(4)]] float3 fragSmoothNormal;
    // The vertex normal unbent, which the outline is coloured by.
    [[vk::location(5)]] float3 fragGeometryNormal;
    [[vk::location(6)]] nointerpolation uint fragToonIndex;
    // Unjittered clip positions now and last frame, for the velocity (as triangle.vert's).
    [[vk::location(7)]] float4 fragCurrClip;
    [[vk::location(8)]] float4 fragPrevClip;
};

[shader("vertex")]
VertexOutput main(VertexInput input)
{
    VertexOutput output;
    ToonMaterial material = toonMaterials[drawData.toonIndex];
    float4 worldPosition = mul(drawData.model, float4(input.inPosition, 1.0));

    // As triangle.vert: the normal matrix of a TRS model is each column over its squared length.
    float3 mc0 = ColumnOf(drawData.model, 0).xyz;
    float3 mc1 = ColumnOf(drawData.model, 1).xyz;
    float3 mc2 = ColumnOf(drawData.model, 2).xyz;
    float3x3 normalMatrix = MatrixFromColumns(
        mc0 / max(dot(mc0, mc0), 1e-6),
        mc1 / max(dot(mc1, mc1), 1e-6),
        mc2 / max(dot(mc2, mc2), 1e-6));
    float3 normal = normalize(mul(normalMatrix, input.inNormal));
    float3 smoothNormal = dot(input.inOutlineNormal, input.inOutlineNormal) > 0.25 ? normalize(mul(normalMatrix, input.inOutlineNormal)) : normal;
    float faceMask = ToonFaceMask(material, input.inTexCoord);

    // The face normal fix: on the face, the normal leans toward the face's forward (flatten) or out
    // from a sphere around the head, by the fix's method, so the face's light reads as one plane.
    float3 shadingNormal = normal;
    float fixAmount = material.faceSdf2.y * faceMask;
    if (fixAmount > 0.0)
    {
        float3 forward = ToonFaceForward(material);
        float3 fromHead = worldPosition.xyz - ToonHeadPosition(material);
        float3 target = forward + material.faceSdf2.z * (fromHead - forward);
        shadingNormal = normalize(lerp(normal, target, fixAmount));
    }

    if (kOutline)
    {
        // The width grows with distance and field of view up to a cap (Unity's 60 for the product
        // of the distance and the vertical field of view in degrees), so the line keeps its width on
        // screen near the camera and thins far away.
        float distance = -mul(ubo.view, worldPosition).z;
        float fovDegrees = degrees(2.0 * atan(1.0 / max(abs(ubo.proj[1][1]), 1e-6)));
        float scale = min(abs(distance) * fovDegrees, 60.0);
        float width = material.outline.x;
        if (ToonHas(material, TOON_FEATURE_OUTLINE_WIDTH_TEXTURE))
        {
            width *= toonOutlineWidthMap.SampleLevel(input.inTexCoord, 0.0).g;
        }
        worldPosition.xyz += smoothNormal * (width * scale * 0.00005);

        // Pushed away from the camera by the outline's z offset (the face's own on the face), its
        // depth alone, so it hides behind the surface it outlines without moving on screen.
        float4 viewPosition = mul(ubo.view, worldPosition);
        float4 clip = mul(ubo.proj, viewPosition);
        float zOffset = lerp(material.outline.y, material.outline.z, faceMask) * max(scale * 0.02, 1.0) + material.depthTexRim2.z;
        float4 pushed = mul(ubo.proj, float4(viewPosition.xy, viewPosition.z - zOffset, 1.0));
        output.position = float4(clip.xy, pushed.z / pushed.w * clip.w, clip.w);
    }
    else
    {
        output.position = mul(mul(ubo.proj, ubo.view), worldPosition);
    }

    output.fragWorldPosition = worldPosition.xyz;
    output.fragWorldNormal = shadingNormal;
    output.fragGeometryNormal = normal;
    output.fragSmoothNormal = smoothNormal;
    output.fragTexCoord = input.inTexCoord;
    output.fragTexCoord1 = input.inTexCoord1;
    output.fragToonIndex = drawData.toonIndex;
    output.fragCurrClip = mul(ubo.viewProjNoJitter, worldPosition);
    output.fragPrevClip = mul(ubo.prevViewProj, mul(previousModels[input.instanceIndex], float4(input.inPreviousPosition, 1.0)));
    return output;
}
