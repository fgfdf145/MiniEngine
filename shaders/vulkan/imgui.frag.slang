// ImGui's fragments (ImGuiNvrhiRenderer): the vertex colour times the texture. ImGui's colours are sRGB
// values, and the SDR swapchain is UNORM, so they are written as they are and blend as a browser
// blends them.
//
// IMGUI_HDR10 is the shader for an HDR10 swapchain, drawing into HdrUiComposite's float layer: the
// colours are decoded to linear light (the scene image is display-linear already, see tonemap.frag)
// and the product, 1.0 being SDR white and the scene's highlights above it, is written sRGB-encoded
// (extended past 1). The UI's own 8-bit textures are sRGB-encoded already and go through as in SDR. The layer blends in that encoding, exactly as the SDR swapchain does, so
// translucent UI looks the same in HDR; hdr_ui_encode.frag then puts it on the display in PQ.
#include "d3d_register.slang"
#ifdef IMGUI_HDR10
#include "gt7_tonemap.slang"
#include "hdr_output.slang"
#endif

[[vk::binding(0, 0)]] Texture2D imguiTexture : register(t0, space0);
[[vk::binding(64, 0)]] SamplerState imguiSampler : register(s64, space0);

#ifdef IMGUI_HDR10
// Must match imgui.vert's and ImGuiConstants in engine/renderer/vulkan/imgui_nvrhi.cpp.
struct ImGuiConstants
{
    float2 scale;
    float2 translate;
    uint linearTexture;
    uint padding0;
    uint padding1;
    uint padding2;
};

[[vk::push_constant]] ConstantBuffer<ImGuiConstants> constants D3D_PUSH_CONSTANTS(0);

float3 SrgbToLinear(float3 encoded)
{
    return select(encoded > float3(0.04045), pow((encoded + 0.055) / 1.055, float3(2.4)), encoded / 12.92);
}
#endif

[shader("fragment")]
// imgui.vert's outputs in its order (Direct3D matches the stages by signature position).
float4 main(float4 fragCoord : SV_Position, [[vk::location(0)]] float4 inColor : COLOR0, [[vk::location(1)]] float2 inUV : TEXCOORD0) : SV_Target0
{
#ifdef IMGUI_HDR10
    float4 texel = imguiTexture.Sample(imguiSampler, inUV);
    if (constants.linearTexture == 0u)
    {
        // The UI's own textures hold what the SDR swapchain shows: the same product as there.
        return inColor * texel;
    }
    float4 color = float4(SrgbToLinear(inColor.rgb), inColor.a) * texel;
    return float4(SrgbEncodeExtended3(max(color.rgb, float3(0.0))), color.a);
#else
    return inColor * imguiTexture.Sample(imguiSampler, inUV);
#endif
}
