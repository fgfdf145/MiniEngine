#pragma once

#include <engine/editor/ui/framework/editor_window.h>

namespace me
{

// Edit > Preferences: the UI scale, the audio output and the way to the Theme and Graphics Debug
// panels.
class PreferencesWindow final : public EditorWindow
{
  public:
    PreferencesWindow();

  protected:
    void OnGui(EditorContext& context) override;
    void PreBegin(EditorContext& context) override;
};
}
