#pragma once

#include <engine/editor/ui/framework/editor_window.h>

namespace me
{

// Help > Keyboard Shortcuts: every command's shortcut, by menu, and the viewport's own keys.
class KeyboardShortcutsWindow final : public EditorWindow
{
  public:
    KeyboardShortcutsWindow();

  protected:
    void OnGui(EditorContext& context) override;
    void PreBegin(EditorContext& context) override;
};
}
