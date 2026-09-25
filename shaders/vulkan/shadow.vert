#version 450

// Depth only: renders a caster into one cascade of the directional shadow map. The C++ side of the
// push constant block is ShadowPushConstants in engine/renderer/vulkan/shadow_pass.h.
layout(push_constant) uniform ShadowConstants
{
    mat4 lightModelViewProjection;
    vec4 baseColorFactor;
    vec4 nodeGraphFactors;
    vec4 alphaCutoffAndPadding; // x = alpha cutoff
    // The base colour's texture transform (material_uv.glsl): (a, b, tx, UV set), (c, d, ty, 0).
    vec4 baseColorTransformRow0;
    vec4 baseColorTransformRow1;
}
shadowData;

layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec2 inTexCoord;
layout(location = 5) in vec2 inTexCoord1;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec2 fragTexCoord1;

void main()
{
    gl_Position = shadowData.lightModelViewProjection * vec4(inPosition, 1.0);
    fragTexCoord = inTexCoord;
    fragTexCoord1 = inTexCoord1;
}
