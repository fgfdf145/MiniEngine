#version 450

// fullscreen.vert's triangle, placed on the far plane: with a LESS_OR_EQUAL depth test it covers
// exactly the pixels no geometry wrote, whose depth is still the 1.0 they were cleared to.
layout(location = 0) out vec2 fragTexCoord;

void main()
{
    const vec2 position = vec2(
        (gl_VertexIndex == 1) ? 3.0 : -1.0,
        (gl_VertexIndex == 2) ? 3.0 : -1.0);
    fragTexCoord = position * 0.5 + 0.5;
    gl_Position = vec4(position, 1.0, 1.0);
}
