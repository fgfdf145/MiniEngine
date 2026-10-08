// A single triangle covering the whole viewport, generated from the vertex index so the pass
// needs no vertex buffer and no vertex input state. Vertices land at (-1,-1), (3,-1), (-1,3),
// which clips to exactly the [-1,1] square.
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

    // Texture coordinates with the origin at the top left, matching the project's UV convention
    // (see the Vulkan UV note in README): NDC y = -1 is the image's top row.
    VertexOutput output;
    output.fragTexCoord = position * 0.5 + 0.5;
    output.position = float4(position, 0.0, 1.0);
    return output;
}
