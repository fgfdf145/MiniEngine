// ImGui's fragment shader for an HDR10 swapchain. ImGui's colours are sRGB values (the SDR swapchain
// is UNORM and shows them as they are), so they are decoded to linear light here; the scene image is
// display-linear already (see tonemap.frag). The product, 1.0 being SDR white, is placed at
// kUiWhiteNits, converted to Rec.2020 and PQ-encoded, so the UI keeps its SDR brightness while the
// scene can exceed it.
// The interface must match the backend's glsl_shader.vert (see imgui_impl_vulkan.cpp):
// since 1.92.8 the texture and the sampler are separate descriptors in sets 0 and 1.

#include "gt7_tonemap.slang"
#include "hdr_output.slang"

[[vk::binding(0, 0)]] Texture2D _Texture;
[[vk::binding(0, 1)]] SamplerState _Sampler;

float3 SrgbToLinear(float3 encoded)
{
    return select(encoded > float3(0.04045), pow((encoded + 0.055) / 1.055, float3(2.4)), encoded / 12.92);
}

// The vertex shader's output block: colour at location 0, UV at location 1.
[shader("fragment")]
float4 main([[vk::location(0)]] float4 inColor, [[vk::location(1)]] float2 inUV) : SV_Target0
{
    float4 color = float4(SrgbToLinear(inColor.rgb), inColor.a) * _Texture.Sample(_Sampler, inUV);
    float3 rec2020 = Rec709ToRec2020(max(color.rgb, float3(0.0)));
    return float4(PqEncodeNits3(rec2020 * kUiWhiteNits), color.a);
}
