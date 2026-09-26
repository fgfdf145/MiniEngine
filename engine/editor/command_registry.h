#pragma once

#include <imgui.h>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace me
{

// One editor action. It is defined once and the menus, the toolbar, the keyboard shortcuts and
// (later) the command palette are all built from it, so no UI code holds the action's logic.
struct Command
{
    std::string id;       // "render.reload_shaders"
    std::string label;    // display name, as the toolbar tooltip and the command palette show it
    std::string menuPath; // "Render/Reload Shaders": top menu, at most one submenu, then the item. Empty: in no menu.
    std::string icon;     // icon font glyph, optional
    ImGuiKeyChord shortcut = 0;
    std::function<void()> execute;
    std::function<bool()> isChecked; // optional; if set, the item is checkable
    std::function<bool()> isEnabled; // optional; enabled when unset
};

bool IsCommandEnabled(const Command& command);
bool IsCommandCheckable(const Command& command);
bool IsCommandChecked(const Command& command);

// The menu tree built from the commands' menuPath. It holds no ImGui state, so the same tree can
// be walked to build ImGui menus or a platform's native menu bar.
struct CommandMenuNode
{
    enum class Kind
    {
        Menu,
        Item,
        Separator
    };

    Kind kind = Kind::Menu;
    std::string label;                     // Menu and Item
    std::size_t commandIndex = 0;          // Item: index into CommandRegistry::GetCommands()
    std::vector<CommandMenuNode> children; // Menu
};

class CommandRegistry
{
  public:
    // Menus and their items keep the order they were first registered in. Returns false, and
    // registers nothing, when the id is empty or taken, or the menuPath is not "Menu/Item" or
    // "Menu/Submenu/Item".
    bool Register(Command command);
    // A separator at the current end of a menu or submenu, e.g. "File" or "Render/Tone Mapping".
    // Creates the menu when it does not exist yet, so it also fixes the order of top-level menus.
    bool AddSeparator(std::string_view menuPath);

    const Command* Find(std::string_view id) const;
    // Runs the command when it exists and is enabled; returns whether it ran.
    bool Execute(std::string_view id) const;
    bool Execute(const Command& command) const;

    // Every command in registration order: what the command palette searches.
    const std::vector<Command>& GetCommands() const
    {
        return m_commands;
    }
    const CommandMenuNode& GetMenuRoot() const
    {
        return m_menuRoot;
    }

  private:
    std::vector<Command> m_commands;
    std::unordered_map<std::string, std::size_t> m_indexById;
    CommandMenuNode m_menuRoot;
};

// "Ctrl+Shift+P"; empty for 0. Needs a current ImGui context.
std::string FormatShortcut(ImGuiKeyChord shortcut);
}
