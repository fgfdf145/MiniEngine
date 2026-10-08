#include "reverse_depth.slang"

// fullscreen.vert's triangle, placed on the far plane: with a nearer-or-equal depth test it covers
// exactly the pixels no geometry wrote, whose depth is still the DEPTH_FAR they were cleared to.
struct VertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 fragTexCoord;
};

[shader("vertex")]
VertexOutput main(uint vertexIndex : SV_VulkanVertexID)
{
    const float2 position = float2(
        (vertexIndex == 1) ? 3.0 : -1.0,
        (vertexIndex == 2) ? 3.0 : -1.0);
    VertexOutput output;
    output.fragTexCoord = position * 0.5 + 0.5;
    output.position = float4(position, DEPTH_FAR, 1.0);
    return output;
}
