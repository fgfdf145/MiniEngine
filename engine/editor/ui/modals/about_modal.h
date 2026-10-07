#pragma once

#include <engine/editor/ui/framework/editor_modal.h>

namespace me
{

// Help > About MiniEngine: the version, the libraries and the project folder.
class AboutModal final : public EditorModal
{
  public:
    AboutModal();

  protected:
    void OnGui(EditorContext& context) override;
};
}
