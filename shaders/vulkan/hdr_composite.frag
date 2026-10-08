#version 450
#extension GL_GOOGLE_include_directive : require

// HDR output: the swapchain's only writer. Puts the SDR UI layer over the viewport's HDR scene image
// (CompositeUiOverScene) and encodes the result, 1.0 being SDR white, for the swapchain: scRGB at
// sdrWhiteNits / 80 per unit, or HDR10 (PQ, Rec.2020) at sdrWhiteNits.

#include "gt7_tonemap.glsl"
#include "hdr_output.glsl"

// Must match HdrCompositePushConstants in engine/renderer/vulkan/hdr_composite.cpp.
layout(push_constant) uniform HdrCompositeConstants
{
    // The viewport image's rectangle in framebuffer pixels (min inclusive, max exclusive) and the
    // scene image's texture coordinates at its corners. An empty rectangle shows no scene.
    vec2 imageMin;
    vec2 imageMax;
    vec2 uvMin;
    vec2 uvMax;
    // scRGB: the scRGB value of SDR white (sdrWhiteNits / 80). HDR10: sdrWhiteNits.
    float outputScale;
    // 0 scRGB, 1 HDR10.
    uint encoding;
}
constants;

// The UI layer, read texel for texel; the scene image, filtered as ImGui would sample it.
layout(set = 0, binding = 0) uniform sampler2D uiLayer;
layout(set = 0, binding = 1) uniform sampler2D sceneImage;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

void main()
{
    vec4 ui = texelFetch(uiLayer, ivec2(gl_FragCoord.xy), 0);
    vec3 color = SrgbDecodeExtended3(ui.rgb);
    if (ui.a < 1.0)
    {
        vec3 scene = vec3(0.0);
        vec2 position = gl_FragCoord.xy;
        if (all(greaterThanEqual(position, constants.imageMin)) && all(lessThan(position, constants.imageMax)))
        {
            vec2 t = (position - constants.imageMin) / (constants.imageMax - constants.imageMin);
            scene = max(texture(sceneImage, mix(constants.uvMin, constants.uvMax, t)).rgb, vec3(0.0));
        }
        color = CompositeUiOverScene(ui, scene);
    }

    if (constants.encoding == 0u)
    {
        outColor = vec4(color * constants.outputScale, 1.0);
    }
    else
    {
        outColor = vec4(PqEncodeNits3(max(color * kRec709ToRec2020, vec3(0.0)) * constants.outputScale), 1.0);
    }
}
