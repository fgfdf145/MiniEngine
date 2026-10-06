#version 450
#extension GL_GOOGLE_include_directive : require

#include "reverse_depth.glsl"

// The selection outline (engine/renderer/vulkan/selection_outline_pass.h): a line around the
// selected entity's silhouette, drawn outside it, as Blender outlines the active object. ImGui draws
// this image over the viewport with straight alpha, so it writes the line's colour and its coverage.

// Reverse-Z depths, DEPTH_FAR where nothing was drawn: the selected entity's own, unjittered, and the
// scene's.
layout(set = 0, binding = 0) uniform sampler2D selectionDepth;
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;

// SelectionOutlinePushConstants in selection_outline_pass.cpp.
layout(push_constant) uniform OutlineConstants
{
    vec4 color;
    float width;
    float occludedOpacity;
    int radius;
    uint enabled;
}
outline;

layout(location = 0) out vec4 outColor;

// How much farther than the scene's surface the entity may be and still count as in view: the scene
// depth is jittered by TAA and the entity's is not, so on a surface of the entity itself the two
// differ slightly. Reverse-Z depth goes as 1 / distance, so this is a fraction of the distance.
const float kVisibleDepthRatio = 0.995;

void main()
{
    outColor = vec4(outline.color.rgb, 0.0);
    if (outline.enabled == 0u)
    {
        return;
    }

    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    // The line lies outside the silhouette; the entity's own pixels are left as they are.
    if (!IsFarDepth(texelFetch(selectionDepth, pixel, 0).r))
    {
        return;
    }

    // The distance to the nearest pixel of the silhouette, and to the nearest one in view.
    const ivec2 size = textureSize(selectionDepth, 0);
    float nearest = 1e6;
    float nearestVisible = 1e6;
    for (int y = -outline.radius; y <= outline.radius; ++y)
    {
        for (int x = -outline.radius; x <= outline.radius; ++x)
        {
            const ivec2 tap = pixel + ivec2(x, y);
            if (any(lessThan(tap, ivec2(0))) || any(greaterThanEqual(tap, size)))
            {
                continue;
            }
            const float entityDepth = texelFetch(selectionDepth, tap, 0).r;
            if (IsFarDepth(entityDepth))
            {
                continue;
            }
            const float distance = length(vec2(x, y));
            nearest = min(nearest, distance);
            if (entityDepth >= texelFetch(sceneDepth, tap, 0).r * kVisibleDepthRatio)
            {
                nearestVisible = min(nearestVisible, distance);
            }
        }
    }

    // The silhouette's edge is half a pixel short of the nearest inside pixel's centre, so the line
    // covers this pixel fully up to width pixels from it and fades over the next one.
    const float visible = clamp(outline.width + 1.0 - nearestVisible, 0.0, 1.0);
    const float hidden = clamp(outline.width + 1.0 - nearest, 0.0, 1.0) * outline.occludedOpacity;
    outColor.a = max(visible, hidden);
}
