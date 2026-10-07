#version 450
#extension GL_GOOGLE_include_directive : require

// The toon prepass (toon_pass.h): an anime character's linear view depth, the depth texture the toon
// pass's rim light and contact shadow read, and the eye mask. As Unity's depth-only pass, the face
// is written a little farther than it is, so the nose and the hair on the face cast the contact
// shadow and not the face onto itself.
layout(constant_id = 0) const bool kOutline = false;

#include "toon_common.glsl"

layout(location = 0) in vec3 fragWorldPosition;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 6) flat in uint fragToonIndex;

layout(location = 0) out float outLinearDepth;
layout(location = 1) out float outMask;

void main()
{
    ToonMaterial material = toonMaterials.materials[fragToonIndex];
    vec3 toCamera = normalize(ubo.cameraWorldPosition.xyz - fragWorldPosition);
    float baseAlpha = texture(toonBaseMap, fragTexCoord, MATERIAL_MIP_BIAS).a * material.baseColor.a;
    ToonAlpha(material, baseAlpha, fragTexCoord, toCamera, 0.0, ToonHas(material, TOON_FEATURE_ALPHA_TEST));

    float faceMask = ToonFaceMask(material, fragTexCoord);
    outLinearDepth = -(ubo.view * vec4(fragWorldPosition, 1.0)).z + faceMask * material.depthTexShadowTintFace.w;
    outMask = ToonHas(material, TOON_FEATURE_STENCIL_WRITE) ? 1.0 : 0.0;
}
