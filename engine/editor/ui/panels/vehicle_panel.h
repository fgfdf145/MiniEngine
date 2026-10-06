#pragma once

#include <engine/editor/ui/framework/editor_panel.h>

namespace me
{

// Play mode's car: drive the selected model, its telemetry, controls, camera, gamepad feedback,
// physics overlay and tuning (EditorSharedState::vehicle).
class VehiclePanel final : public EditorPanel
{
  public:
    VehiclePanel();

  protected:
    void OnGui(EditorContext& context) override;
};
}
