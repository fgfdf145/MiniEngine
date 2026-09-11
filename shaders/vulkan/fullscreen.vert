#version 450

// A single triangle covering the whole viewport, generated from the vertex index so the pass
// needs no vertex buffer and no vertex input state. Vertices land at (-1,-1), (3,-1), (-1,3),
// which clips to exactly the [-1,1] square.
layout(location = 0) out vec2 fragTexCoord;

void main()
{
    const vec2 position = vec2(
        (gl_VertexIndex == 1) ? 3.0 : -1.0,
        (gl_VertexIndex == 2) ? 3.0 : -1.0);

    // Texture coordinates with the origin at the top left, matching the project's UV convention
    // (see the Vulkan UV note in README): NDC y = -1 is the image's top row.
    fragTexCoord = position * 0.5 + 0.5;
    gl_Position = vec4(position, 0.0, 1.0);
}
