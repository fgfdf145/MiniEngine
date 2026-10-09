// HdrUiComposite's last step on an HDR10 swapchain: the editor frame, drawn sRGB-encoded into a float
// layer (imgui.frag's HDR10 variant), decoded to linear light, 1.0 placed at the UI white (Windows'
// SDR content brightness), converted to Rec.2020 and PQ-encoded.
#include "d3d_register.slang"
#include "gt7_tonemap.slang"
#include "hdr_output.slang"

[[vk::binding(0, 0)]] Texture2D uiLayer : register(t0, space0);

// Must match HdrUiEncodeConstants in engine/renderer/vulkan/hdr_ui_composite.cpp.
struct HdrUiEncodeConstants
{
    float uiWhiteNits;
    float padding0;
    float padding1;
    float padding2;
};
[[vk::push_constant]] ConstantBuffer<HdrUiEncodeConstants> constants D3D_PUSH_CONSTANTS(0);

[shader("fragment")]
// fullscreen.vert's outputs in its order (Direct3D matches the stages by signature position).
float4 main(float4 fragCoord : SV_Position, [[vk::location(0)]] float2 fragTexCoord : TEXCOORD0) : SV_Target0
{
    // One texel per pixel: the layer is the swapchain's size.
    float3 encoded = uiLayer.Load(int3(int2(fragCoord.xy), 0)).rgb;
    float3 rec2020 = Rec709ToRec2020(SrgbDecodeExtended3(encoded));
    return float4(PqEncodeNits3(rec2020 * constants.uiWhiteNits), 1.0);
}
