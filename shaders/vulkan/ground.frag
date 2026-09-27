#version 450
#extension GL_GOOGLE_include_directive : require

// The ground plane (ground_plane.glsl) into the G-buffer: a full-screen triangle whose pixels find
// the plane along their view ray and write its depth, so the geometry pass's depth test puts it
// behind whatever the draw items covered. Pixels that miss it, or would land past the far plane,
// stay background and show the sky's own ground (atmosphere_sampling.glsl).

#include "scene_common.glsl"
#include "gbuffer_common.glsl"
#include "ground_plane.glsl"

layout(location = 0) in vec2 fragTexCoord;

// As gbuffer.frag.
layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outSurface;
layout(location = 3) out vec4 outEmissive;
layout(location = 4) out vec4 outVelocity;
layout(location = 5) out vec4 outSpecular;
layout(location = 6) out vec4 outCoat;
layout(location = 7) out vec4 outSheen;

void main()
{
    vec3 camera = ubo.cameraWorldPosition.xyz;
    vec4 farPoint = ubo.invViewProj * vec4(fragTexCoord * 2.0 - 1.0, 1.0, 1.0);
    vec3 direction = normalize(farPoint.xyz / farPoint.w - camera);
    float t;
    bool frontFace;
    if (!IntersectGroundPlane(camera, direction, 0.0, 1e30, t, frontFace) || !frontFace)
    {
        discard;
    }
    vec4 position = vec4(camera + t * direction, 1.0);
    vec4 clip = ubo.proj * ubo.view * position;
    float depth = clip.z / clip.w;
    if (depth >= 1.0)
    {
        discard;
    }
    gl_FragDepth = depth;

    vec2 up = EncodeNormalOctahedral(vec3(0.0, 1.0, 0.0));
    outAlbedo = vec4(ubo.groundAlbedo.rgb, 1.0);
    outNormal = vec4(up, up);
    outSurface = vec4(0.0, GROUND_PLANE_ROUGHNESS, 1.0, EncodeShadingFlags(0u));
    outEmissive = vec4(0.0);
    vec4 currClip = ubo.viewProjNoJitter * position;
    vec4 prevClip = ubo.prevViewProj * position;
    outVelocity = vec4((currClip.xy / currClip.w - prevClip.xy / prevClip.w) * 0.5, 0.0, 0.0);
    outSpecular = vec4(0.0);
    outCoat = vec4(0.0);
    outSheen = vec4(0.0);
}
