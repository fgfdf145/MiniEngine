#pragma once

#include <engine/editor/ui/framework/editor_panel.h>

namespace me
{

// The editor camera: where it is, how it moves, its lens and its exposure, and the UI scale.
class CameraPanel final : public EditorPanel
{
  public:
    CameraPanel();

  protected:
    void OnGui(EditorContext& context) override;
};
}
