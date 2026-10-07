#pragma once

#include <engine/editor/ui/framework/editor_window.h>
#include <engine/platform/process/process_allocation.h>

#include <optional>

namespace me
{

// Edit > Preferences: the UI scale, the audio output, the process's priority and CPUs, and the way to
// the Theme and Graphics Debug panels.
class PreferencesWindow final : public EditorWindow
{
  public:
    PreferencesWindow();

  protected:
    void OnGui(EditorContext& context) override;
    void PreBegin(EditorContext& context) override;

  private:
    void DrawProcessSection(EditorContext& context);
    void DrawCpuGrid(platform::process::ProcessAllocation& allocation);

    // Read once: the CPUs do not change while the engine runs.
    std::optional<platform::process::ProcessorTopology> m_topology;
};
}
