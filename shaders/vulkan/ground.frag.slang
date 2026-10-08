// The ground plane (ground_plane.slang) into the G-buffer: a full-screen triangle whose pixels find
// the plane along their view ray and write its depth, so the geometry pass's depth test puts it
// behind whatever the draw items covered. Pixels that miss it, or would land past the far plane,
// stay background and show the sky's own ground (atmosphere_sampling.slang).

#include "scene_common.slang"
#include "gbuffer_common.slang"
#include "ground_plane.slang"

struct FragmentInput
{
    [[vk::location(0)]] float2 fragTexCoord;
};

// As gbuffer.frag, plus the plane's depth.
struct FragmentOutput
{
    float fragDepth : SV_Depth;
    [[vk::location(0)]] float4 outAlbedo : SV_Target0;
    [[vk::location(1)]] float4 outNormal : SV_Target1;
    [[vk::location(2)]] float4 outSurface : SV_Target2;
    [[vk::location(3)]] float4 outEmissive : SV_Target3;
    [[vk::location(4)]] float4 outVelocity : SV_Target4;
    [[vk::location(5)]] float4 outSpecular : SV_Target5;
    [[vk::location(6)]] float4 outCoat : SV_Target6;
    [[vk::location(7)]] float4 outSheen : SV_Target7;
};

[shader("fragment")]
FragmentOutput main(FragmentInput input)
{
    FragmentOutput output;
    float3 camera = ubo.cameraWorldPosition.xyz;
    float4 farPoint = mul(ubo.invViewProj, float4(input.fragTexCoord * 2.0 - 1.0, DEPTH_FAR, 1.0));
    float3 direction = normalize(farPoint.xyz / farPoint.w - camera);
    float t;
    bool frontFace;
    if (!IntersectGroundPlane(camera, direction, 0.0, 1e30, t, frontFace) || !frontFace)
    {
        discard;
    }
    float4 position = float4(camera + t * direction, 1.0);
    float4 clip = mul(mul(ubo.proj, ubo.view), position);
    float depth = clip.z / clip.w;
    if (IsFarDepth(depth))
    {
        discard;
    }
    output.fragDepth = depth;

    float2 up = EncodeNormalOctahedral(float3(0.0, 1.0, 0.0));
    output.outAlbedo = float4(ubo.groundAlbedo.rgb, 1.0);
    output.outNormal = float4(up, up);
    output.outSurface = float4(0.0, GROUND_PLANE_ROUGHNESS, 1.0, EncodeShadingFlags(0u));
    output.outEmissive = float4(0.0);
    float4 currClip = mul(ubo.viewProjNoJitter, position);
    float4 prevClip = mul(ubo.prevViewProj, position);
    output.outVelocity = float4((currClip.xy / currClip.w - prevClip.xy / prevClip.w) * 0.5, 0.0, 0.0);
    output.outSpecular = float4(0.0);
    output.outCoat = float4(0.0);
    output.outSheen = float4(0.0);
    return output;
}
