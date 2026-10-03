#pragma once

// The editor's main menu bar and toolbar, both generated from a CommandRegistry.

#include <engine/editor/command_registry.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace me
{

struct ToolbarItem
{
    // A button that runs this command. Checked commands show as pressed.
    std::string commandId;
    // Set instead of commandId: a dropdown over these commands, showing the checked one. Meant
    // for a radio group, such as the debug views.
    std::vector<std::string> dropdownCommandIds;
    std::string dropdownTooltip;
    float dropdownWidth = 0.0f; // unscaled pixels

    static ToolbarItem Button(std::string commandId)
    {
        ToolbarItem item;
        item.commandId = std::move(commandId);
        return item;
    }
    static ToolbarItem Dropdown(std::string tooltip, std::vector<std::string> commandIds, float width)
    {
        ToolbarItem item;
        item.dropdownTooltip = std::move(tooltip);
        item.dropdownCommandIds = std::move(commandIds);
        item.dropdownWidth = width;
        return item;
    }
};

// Related items sit side by side; groups are set apart by a wider gap.
using ToolbarGroup = std::vector<ToolbarItem>;

struct ToolbarLayout
{
    std::vector<ToolbarGroup> left;   // aligned to the left edge
    std::vector<ToolbarGroup> center; // centered in the bar
    std::vector<ToolbarGroup> right;  // aligned to the right edge
};

// Runs the command of every shortcut pressed this frame. Call once per frame, before the windows
// are drawn. Shortcuts are global, but a focused text field keeps its own (Ctrl+C, Ctrl+Z, ...)
// and a shortcut without Ctrl, Alt or Super is ignored while text is being typed.
void ProcessCommandShortcuts(const CommandRegistry& registry);

// BeginMainMenuBar with a menu for each top-level node of the registry's menu tree.
void DrawMainMenu(const CommandRegistry& registry);

// A bar along the top of the main viewport, below the main menu bar: call after DrawMainMenu and
// before DockSpaceOverViewport so the dock space fills what is left.
void DrawToolbar(const CommandRegistry& registry, const ToolbarLayout& layout, float uiScale);

// How well `query` matches `text`, ignoring case: every query character in order, with consecutive
// characters and characters at the start of a word scoring higher. No value when it does not match;
// an empty query matches everything with 0.
std::optional<int> FuzzyMatchScore(std::string_view query, std::string_view text);

// The commands a palette query lists, best match first: enabled commands only, matched by label or
// menu path. Indices into registry.GetCommands().
std::vector<std::size_t> FindPaletteCommands(const CommandRegistry& registry, std::string_view query);

// A popup at the top of the main viewport with a search over every command. Arrow keys pick, Enter
// or a click runs the command, Escape or a click outside closes it.
class CommandPalette
{
  public:
    void Open()
    {
        m_openRequested = true;
    }
    // Call every frame from the same place, before the command state is applied, so a command it
    // runs takes effect this frame.
    void Draw(const CommandRegistry& registry, float uiScale);

  private:
    bool m_openRequested = false;
    bool m_focusInput = false;
    std::string m_query;
    std::size_t m_selected = 0;
};

// Every command with a shortcut, by menu, plus `extraKeys` ({keys, what they do}) for keys the
// commands do not own, such as the viewport's.
void DrawKeyboardShortcutsWindow(
    const CommandRegistry& registry,
    bool* open,
    std::span<const std::pair<const char*, const char*>> extraKeys);
}
