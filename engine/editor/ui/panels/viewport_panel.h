#pragma once

#include <engine/editor/ui/framework/editor_panel.h>
#include <engine/logic/gizmo_settings.h>

namespace me
{

// The scene as the renderer draws it, with the gizmos, the selection and the overlays (the driving
// HUD, the minimap, the physics overlay). Over the fullscreen viewport it is a borderless window of
// its own, so the docked one keeps its place in the layout.
class ViewportPanel final : public EditorPanel
{
  public:
    ViewportPanel();

    // Drawn while fullscreen even when its tab is closed: it is all there is then.
    bool ShouldDraw(const EditorContext& context) const override;
    bool DrawsInFullscreen() const override
    {
        return true;
    }

  protected:
    void OnGui(EditorContext& context) override;
    void PreBegin(EditorContext& context) override;
    void PostEnd(EditorContext& context) override;
    ImGuiWindowFlags GetWindowFlags(const EditorContext& context) const override;
    const char* GetImGuiName(const EditorContext& context) const override;
    bool IsClosable(const EditorContext& context) const override;

  private:
    GizmoDragSnapState m_gizmoDragSnapState;
    int m_pushedStyleVars = 0;
};
}
