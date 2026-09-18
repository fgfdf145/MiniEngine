#pragma once

#include <cstdint>

namespace me
{

struct RenderExtent
{
    uint32_t width = 0;
    uint32_t height = 0;

    bool IsValid() const
    {
        return width > 0 && height > 0;
    }
};

// What the viewport shows instead of the tone mapped image. The numeric values are the
// tonemap.frag push constant and must match the GBUFFER_VIEW_* constants there.
enum class GBufferDebugView : uint32_t
{
    Off = 0,
    Albedo = 1,
    Normal = 2,
    GeometricNormal = 3,
    Surface = 4,
    Emissive = 5
};

// Renderer switches the editor owns and the backend reads when it builds each frame. Plain data,
// handed over by copy in EditorUiFrameResult.
struct RenderDebugSettings
{
    GBufferDebugView gbufferView = GBufferDebugView::Off;
    // Records the forward-only order instead of the deferred one: the comparison switch that makes
    // pixel equivalence something a reviewer flips rather than judges.
    bool forwardOnly = false;
};
}
