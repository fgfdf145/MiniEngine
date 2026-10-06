#pragma once

#include <engine/editor/ui/framework/editor_panel.h>

namespace me
{

// The scene's entities, the selected one's inspector, the environment and the scene file: the
// editor's outliner and details in one panel.
class ScenePanel final : public EditorPanel
{
  public:
    ScenePanel();

  protected:
    void OnGui(EditorContext& context) override;
};
}
