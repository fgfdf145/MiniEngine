#pragma once

#include <engine/editor/ui/framework/editor_modal.h>

#include <optional>

namespace me
{

// Asks before New Scene or Clear Scene throws the scene's contents away.
class SceneResetModal final : public EditorModal
{
  public:
    enum class Reset
    {
        New,
        Clear
    };

    SceneResetModal();

    void Ask(Reset reset);
    void OnClose(EditorContext& context) override;

  protected:
    void OnGui(EditorContext& context) override;

  private:
    std::optional<Reset> m_pending;
};
}
