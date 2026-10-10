#pragma once

#include <engine/editor/ui/framework/editor_panel.h>

namespace me
{

// Tools > Record Viewport's settings and button: the video's size (the viewport resolution), frame
// rate, format and quality, the folder it is saved to, and the recording as it goes
// (EditorSharedState::viewportRecording, videoRecording).
class RecordingPanel final : public EditorPanel
{
  public:
    RecordingPanel();

  protected:
    void OnGui(EditorContext& context) override;
};
}
