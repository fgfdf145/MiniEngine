#include "keyboard_shortcuts_window.h"

#include <engine/editor/ui/editor_menu_toolbar.h>
#include <engine/editor/ui/framework/editor_context.h>

#include <imgui.h>

#include <array>
#include <utility>

namespace me
{

KeyboardShortcutsWindow::KeyboardShortcutsWindow()
    : EditorWindow("keyboard_shortcuts", "Keyboard Shortcuts")
{
}

void KeyboardShortcutsWindow::PreBegin(EditorContext& context)
{
    static_cast<void>(context);
    ImGui::SetNextWindowSize(ImVec2(460.0f * UiScale(), 520.0f * UiScale()), ImGuiCond_FirstUseEver);
}

void KeyboardShortcutsWindow::OnGui(EditorContext& context)
{
    // Keys the viewport handles itself, not through a command.
    static constexpr std::array<std::pair<const char*, const char*>, 6> kViewportKeys = {{
        {"W A S D", "Move the camera"},
        {"Right mouse", "Look around"},
        {"Alt + Right mouse", "Orbit the selection"},
        {"Middle mouse", "Pan"},
        {"R", "Toggle the combined and scale gizmo"},
        {"Escape", "Leave the fullscreen viewport"},
    }};
    DrawKeyboardShortcutsTable(context.commands, kViewportKeys);
}
}
