#version 450 core
#extension GL_GOOGLE_include_directive : require

// ImGui's fragment shader for an HDR10 swapchain. The backend's own shader writes display-linear
// Rec.709, 1.0 being SDR white, which an sRGB swapchain encodes; here the same value is placed at
// kUiWhiteNits, converted to Rec.2020 and PQ-encoded, so the UI keeps its SDR brightness while the
// scene image (display-linear too, relative to the same white, see tonemap.frag) can exceed it.
// The interface must match the backend's glsl_shader.vert (see imgui_impl_vulkan.cpp).

#include "gt7_tonemap.glsl"
#include "hdr_output.glsl"

layout(location = 0) out vec4 fColor;
layout(set = 0, binding = 0) uniform sampler2D sTexture;
layout(location = 0) in struct
{
    vec4 Color;
    vec2 UV;
} In;

void main()
{
    vec4 color = In.Color * texture(sTexture, In.UV.st);
    vec3 rec2020 = max(color.rgb, vec3(0.0)) * kRec709ToRec2020;
    fColor = vec4(PqEncodeNits3(rec2020 * kUiWhiteNits), color.a);
}
