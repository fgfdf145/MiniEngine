// ImGui's vertices to clip space (ImGuiNvrhiRenderer): pixel positions scaled and translated by the
// draw data's display rectangle, y down as the engine's other vertex shaders write it.
#include "d3d_register.slang"
#include "shader_helpers.slang"

// Must match imgui.frag's and ImGuiConstants in engine/renderer/vulkan/imgui_nvrhi.cpp.
struct ImGuiConstants
{
    float2 scale;
    float2 translate;
    // Whether the draw's texture holds linear light (imgui.frag's HDR10 variant).
    uint linearTexture;
    uint padding0;
    uint padding1;
    uint padding2;
};

[[vk::push_constant]] ConstantBuffer<ImGuiConstants> constants D3D_PUSH_CONSTANTS(0);

struct VertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput main(
    [[vk::location(0)]] float2 position : POSITION,
    [[vk::location(1)]] float2 uv : TEXCOORD0,
    [[vk::location(2)]] float4 color : COLOR0)
{
    VertexOutput output;
    output.position = ClipPosition(float4(position * constants.scale + constants.translate, 0.0, 1.0));
    output.color = color;
    output.uv = uv;
    return output;
}
