#pragma once

#include <engine/editor/ui/framework/editor_panel.h>

namespace me
{

// Photo Mode (docs/design/2026-10-09-photo-mode-design.md): a still from the viewport's camera at a
// resolution of its own, which leaves the viewport's alone: the photo's size (presets or custom),
// how many frames its view renders before it is saved, the viewport's framing guide, and the button
// (EditorSharedState::photoMode, photoStatus).
class PhotoModePanel final : public EditorPanel
{
  public:
    PhotoModePanel();

  protected:
    void OnGui(EditorContext& context) override;
};
}
