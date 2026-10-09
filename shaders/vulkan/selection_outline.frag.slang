#include "d3d_register.slang"
#include "shader_helpers.slang"
#include "reverse_depth.slang"

// The selection outline (engine/renderer/vulkan/selection_outline_pass.h): a line around the
// selected entity's silhouette, drawn outside it, as Blender outlines the active object. ImGui draws
// this image over the viewport with straight alpha, so it writes the line's colour and its coverage.

// Reverse-Z depths, DEPTH_FAR where nothing was drawn: the selected entity's own, unjittered, and the
// scene's.
[[vk::binding(0, 0)]] Texture2D selectionDepth : register(t0, space0);
[[vk::binding(1, 0)]] Texture2D sceneDepth : register(t1, space0);

// SelectionOutlinePushConstants in selection_outline_pass.cpp.
struct OutlineConstants
{
    float4 color;
    float width;
    float occludedOpacity;
    int radius;
    uint enabled;
};
[[vk::push_constant]] ConstantBuffer<OutlineConstants> outline D3D_PUSH_CONSTANTS(0);

// How much farther than the scene's surface the entity may be and still count as in view: the scene
// depth is jittered by TAA and the entity's is not, so on a surface of the entity itself the two
// differ slightly. Reverse-Z depth goes as 1 / distance, so this is a fraction of the distance.
static const float kVisibleDepthRatio = 0.995;

struct FragmentInput
{
    float4 fragCoord : SV_Position;
};

struct FragmentOutput
{
    [[vk::location(0)]] float4 outColor : SV_Target0;
};

[shader("fragment")]
FragmentOutput main(FragmentInput input)
{
    FragmentOutput output;
    output.outColor = float4(outline.color.rgb, 0.0);
    if (outline.enabled == 0u)
    {
        return output;
    }

    const int2 pixel = int2(input.fragCoord.xy);
    // The line lies outside the silhouette; the entity's own pixels are left as they are.
    if (!IsFarDepth(selectionDepth.Load(int3(pixel, 0)).r))
    {
        return output;
    }

    // The distance to the nearest pixel of the silhouette, and to the nearest one in view.
    const int2 size = TextureSize(selectionDepth, 0);
    // The scene's depth is at the render size, which DLSS upscales from; the outline's at the output's.
    const int2 sceneSize = TextureSize(sceneDepth, 0);
    float nearest = 1e6;
    float nearestVisible = 1e6;
    for (int y = -outline.radius; y <= outline.radius; ++y)
    {
        for (int x = -outline.radius; x <= outline.radius; ++x)
        {
            const int2 tap = pixel + int2(x, y);
            if (any(tap < int2(0)) || any(tap >= size))
            {
                continue;
            }
            const float entityDepth = selectionDepth.Load(int3(tap, 0)).r;
            if (IsFarDepth(entityDepth))
            {
                continue;
            }
            const float distance = length(float2(x, y));
            nearest = min(nearest, distance);
            if (entityDepth >= sceneDepth.Load(int3(tap * sceneSize / size, 0)).r * kVisibleDepthRatio)
            {
                nearestVisible = min(nearestVisible, distance);
            }
        }
    }

    // The silhouette's edge is half a pixel short of the nearest inside pixel's centre, so the line
    // covers this pixel fully up to width pixels from it and fades over the next one.
    const float visible = clamp(outline.width + 1.0 - nearestVisible, 0.0, 1.0);
    const float hidden = clamp(outline.width + 1.0 - nearest, 0.0, 1.0) * outline.occludedOpacity;
    output.outColor.a = max(visible, hidden);
    return output;
}
