#version 450 core
#extension GL_GOOGLE_include_directive : require

// ImGui's fragment shader for an HDR10 swapchain. ImGui's colours are sRGB values (the SDR swapchain
// is UNORM and shows them as they are), so they are decoded to linear light here; the scene image is
// display-linear already (see tonemap.frag). The product, 1.0 being SDR white, is placed at
// kUiWhiteNits, converted to Rec.2020 and PQ-encoded, so the UI keeps its SDR brightness while the
// scene can exceed it.
// The interface must match the backend's glsl_shader.vert (see imgui_impl_vulkan.cpp):
// since 1.92.8 the texture and the sampler are separate descriptors in sets 0 and 1.

#include "gt7_tonemap.glsl"
#include "hdr_output.glsl"

layout(location = 0) out vec4 fColor;
layout(set = 0, binding = 0) uniform texture2D _Texture;
layout(set = 1, binding = 0) uniform sampler _Sampler;
layout(location = 0) in struct
{
    vec4 Color;
    vec2 UV;
} In;

vec3 SrgbToLinear(vec3 encoded)
{
    return mix(encoded / 12.92, pow((encoded + 0.055) / 1.055, vec3(2.4)), greaterThan(encoded, vec3(0.04045)));
}

void main()
{
    vec4 color = vec4(SrgbToLinear(In.Color.rgb), In.Color.a) * texture(sampler2D(_Texture, _Sampler), In.UV.st);
    vec3 rec2020 = max(color.rgb, vec3(0.0)) * kRec709ToRec2020;
    fColor = vec4(PqEncodeNits3(rec2020 * kUiWhiteNits), color.a);
}
