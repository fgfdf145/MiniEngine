#include "shader_helpers.slang"
#include "draw_parameters.slang"
#include "reverse_depth.slang"

// fullscreen.vert's triangle, placed on the far plane: with a nearer-or-equal depth test it covers
// exactly the pixels no geometry wrote, whose depth is still the DEPTH_FAR they were cleared to.
struct VertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 fragTexCoord : TEXCOORD0;
};

[shader("vertex")]
VertexOutput main(DRAW_VERTEX_INDEX_PARAMETERS)
{
    const float2 position = float2(
        (DRAW_VERTEX_INDEX == 1) ? 3.0 : -1.0,
        (DRAW_VERTEX_INDEX == 2) ? 3.0 : -1.0);
    VertexOutput output;
    output.fragTexCoord = position * 0.5 + 0.5;
    output.position = ClipPosition(float4(position, DEPTH_FAR, 1.0));
    return output;
}
