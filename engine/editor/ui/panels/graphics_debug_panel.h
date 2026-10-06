#pragma once

#include <engine/editor/ui/framework/editor_panel.h>

namespace me
{

// The renderer's switches and debug views (EditorSharedState::renderDebug). The settings keep
// applying while the panel is closed: closing a debug window hides its controls, it does not reset
// the renderer.
class GraphicsDebugPanel final : public EditorPanel
{
  public:
    GraphicsDebugPanel();

  protected:
    void OnGui(EditorContext& context) override;
};
}
