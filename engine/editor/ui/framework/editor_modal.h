#pragma once

#include "editor_window.h"

#include <string>
#include <utility>

namespace me
{

// A modal popup centred on the main viewport: Open() asks for it, and it stays until its own
// buttons close it (CloseModal) or Escape dismisses it. Either way the manager calls OnClose, so a
// modal that holds a pending choice drops it there unless a button already took it.
class EditorModal : public EditorWindow
{
  public:
    EditorModal(std::string id, std::string title)
        : EditorWindow(std::move(id), std::move(title))
    {
    }

    // The popup opens when it is next drawn, in the manager's ID stack.
    void Open() override;

    // Modals show over the fullscreen viewport too.
    bool DrawsInFullscreen() const override
    {
        return true;
    }
    // Drawn every frame while asked for or open, to see whether ImGui still has it open.
    bool ShouldDraw(const EditorContext& context) const override;

  protected:
    ImGuiWindowFlags GetWindowFlags(const EditorContext& context) const override
    {
        static_cast<void>(context);
        return ImGuiWindowFlags_AlwaysAutoResize;
    }
    // From inside OnGui: closes the popup. OnClose follows on the next frame.
    void CloseModal();

    bool BeginWindow(EditorContext& context) override;
    void EndWindow(bool contentsDrawn) override;

  private:
    bool m_openRequested = false;
};
}
