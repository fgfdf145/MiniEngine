#version 450
#extension GL_GOOGLE_include_directive : require

#include "reverse_depth.glsl"

// fullscreen.vert's triangle, placed on the far plane: with a nearer-or-equal depth test it covers
// exactly the pixels no geometry wrote, whose depth is still the DEPTH_FAR they were cleared to.
layout(location = 0) out vec2 fragTexCoord;

void main()
{
    const vec2 position = vec2(
        (gl_VertexIndex == 1) ? 3.0 : -1.0,
        (gl_VertexIndex == 2) ? 3.0 : -1.0);
    fragTexCoord = position * 0.5 + 0.5;
    gl_Position = vec4(position, DEPTH_FAR, 1.0);
}
