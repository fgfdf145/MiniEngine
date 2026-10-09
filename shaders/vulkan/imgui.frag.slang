// ImGui's fragments (ImGuiNvrhiRenderer): the vertex colour times the texture. ImGui's colours are sRGB
// values, and the SDR swapchain is UNORM, so they are written as they are and blend as a browser
// blends them.
//
// IMGUI_HDR10 is the shader for an HDR10 swapchain: the colours are decoded to linear light (the
// scene image is display-linear already, see tonemap.frag), the product, 1.0 being SDR white, is
// placed at kUiWhiteNits, converted to Rec.2020 and PQ-encoded, so the UI keeps its SDR brightness
// while the scene can exceed it.
#include "d3d_register.slang"
#ifdef IMGUI_HDR10
#include "gt7_tonemap.slang"
#include "hdr_output.slang"
#endif

[[vk::binding(0, 0)]] Texture2D imguiTexture : register(t0, space0);
[[vk::binding(64, 0)]] SamplerState imguiSampler : register(s64, space0);

#ifdef IMGUI_HDR10
float3 SrgbToLinear(float3 encoded)
{
    return select(encoded > float3(0.04045), pow((encoded + 0.055) / 1.055, float3(2.4)), encoded / 12.92);
}
#endif

[shader("fragment")]
float4 main([[vk::location(0)]] float4 inColor : COLOR0, [[vk::location(1)]] float2 inUV : TEXCOORD0) : SV_Target0
{
#ifdef IMGUI_HDR10
    float4 color = float4(SrgbToLinear(inColor.rgb), inColor.a) * imguiTexture.Sample(imguiSampler, inUV);
    float3 rec2020 = Rec709ToRec2020(max(color.rgb, float3(0.0)));
    return float4(PqEncodeNits3(rec2020 * kUiWhiteNits), color.a);
#else
    return inColor * imguiTexture.Sample(imguiSampler, inUV);
#endif
}
