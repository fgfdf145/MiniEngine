#pragma once

#include <engine/editor/ui/framework/editor_panel.h>

namespace me
{

// The editor palette, edited live (EditorStyle). A change marks the editor settings to be saved.
class ThemePanel final : public EditorPanel
{
  public:
    ThemePanel();

  protected:
    void OnGui(EditorContext& context) override;
};
}
