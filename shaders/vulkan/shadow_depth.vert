#version 450

// Depth only, for opaque casters: shadow.vert without the UVs the alpha test needs, reading the
// position-only stream (VulkanBuffer::GetPositionHandle). The block is shadow.vert's; only its first
// member is read.
layout(push_constant) uniform ShadowConstants
{
    mat4 lightModelViewProjection;
}
shadowData;

layout(location = 0) in vec3 inPosition;

void main()
{
    gl_Position = shadowData.lightModelViewProjection * vec4(inPosition, 1.0);
}
