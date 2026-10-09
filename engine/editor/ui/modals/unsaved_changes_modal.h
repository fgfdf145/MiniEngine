#pragma once

#include <engine/editor/ui/framework/editor_modal.h>

#include <optional>
#include <string>

namespace me
{

// Asks what to do with the scene's unsaved changes before they would be lost: save them first, drop
// them, or stay. Then closes the editor, opens another scene or starts a new one, as was asked.
class UnsavedChangesModal final : public EditorModal
{
  public:
    enum class Then
    {
        Quit,
        OpenScene,
        NewScene
    };

    UnsavedChangesModal();

    // `scenePath` is the scene to open for Then::OpenScene.
    void Ask(Then then, std::string scenePath = {});
    void OnClose(EditorContext& context) override;

  protected:
    void OnGui(EditorContext& context) override;

  private:
    // Goes on with what was asked, the changes saved or dropped.
    void Continue(EditorContext& context);

    std::optional<Then> m_then;
    std::string m_scenePath;
    // Saved this frame: the next frame goes on, unless the save failed.
    bool m_saving = false;
    std::string m_saveError;
};
}
