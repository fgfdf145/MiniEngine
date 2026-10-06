#pragma once

#include <engine/editor/ui/framework/editor_panel.h>

#include <cstdint>
#include <string>
#include <vector>

namespace me
{

// The input events the log captured, newest last.
class InputMonitorPanel final : public EditorPanel
{
  public:
    InputMonitorPanel();

  protected:
    void OnGui(EditorContext& context) override;
    void PreBegin(EditorContext& context) override;

  private:
    bool m_autoScroll = true;
    std::vector<std::string> m_messages;
    uint64_t m_messagesRevision = 0;
};
}
