#pragma once

#include "editor_window.h"

#include <string>
#include <utility>

namespace me
{

// Where a panel goes in the default dock layout (Window > Reset Layout, or the first run).
enum class EditorDockSlot
{
    Floating, // not docked: it floats where it was last left
    Left,
    Center,
    RightBottom,
};

// A dockable tool panel, like Unreal's nomad tabs or Godot's docks: the Window menu lists it and
// toggles it, the editor settings keep whether it is open, and the default layout docks it.
class EditorPanel : public EditorWindow
{
  public:
    EditorPanel(std::string id, std::string title, std::string icon, EditorDockSlot dockSlot = EditorDockSlot::Floating)
        : EditorWindow(std::move(id), std::move(title), std::move(icon)), m_dockSlot(dockSlot)
    {
    }

    EditorDockSlot GetDefaultDockSlot() const
    {
        return m_dockSlot;
    }
    // The key the panel's open state is saved under in the editor settings; its id by default.
    virtual std::string GetSettingsKey() const
    {
        return GetId();
    }

  private:
    EditorDockSlot m_dockSlot;
};
}
