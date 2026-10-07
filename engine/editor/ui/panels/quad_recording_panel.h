#pragma once

#include <engine/editor/ui/framework/editor_panel.h>

namespace me
{

// Films the car from the front, the rear and both sides at once and composes the four pictures into
// one video as it goes (docs/design/2026-10-07-quad-vehicle-recording-design.md): each camera's
// picture size, where it sits and looks from the car, its lens and whether it tilts with the body;
// the canvas's layout; a live preview laid out as the video will be; and the record button
// (EditorSharedState::quadRecording).
class QuadRecordingPanel final : public EditorPanel
{
  public:
    QuadRecordingPanel();

  protected:
    void OnGui(EditorContext& context) override;

  private:
    // The four cameras render for the preview while the window shows it, which costs as much as
    // recording: it can be switched off.
    bool m_preview = true;
};
}
