#include "command_registry.h"

#include <imgui_internal.h>

#include <algorithm>

namespace me
{

namespace
{
// Splits "A/B/C" into its segments. Empty segments make the path invalid, so they are kept.
std::vector<std::string_view> SplitMenuPath(std::string_view menuPath)
{
    std::vector<std::string_view> segments;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t slash = menuPath.find('/', start);
        segments.push_back(menuPath.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));
        if (slash == std::string_view::npos)
        {
            return segments;
        }
        start = slash + 1;
    }
}

bool HasEmptySegment(const std::vector<std::string_view>& segments)
{
    return std::any_of(segments.begin(), segments.end(), [](std::string_view segment)
                       {
                           return segment.empty();
                       });
}

CommandMenuNode& FindOrAddMenu(CommandMenuNode& parent, std::string_view label)
{
    for (CommandMenuNode& child : parent.children)
    {
        if (child.kind == CommandMenuNode::Kind::Menu && child.label == label)
        {
            return child;
        }
    }

    CommandMenuNode& menu = parent.children.emplace_back();
    menu.kind = CommandMenuNode::Kind::Menu;
    menu.label = std::string(label);
    return menu;
}

// Top-level menu, then at most one submenu: deeper menus are hard to navigate.
constexpr std::size_t kMaxMenuDepth = 2;
}

bool IsCommandEnabled(const Command& command)
{
    return !command.isEnabled || command.isEnabled();
}

bool IsCommandCheckable(const Command& command)
{
    return static_cast<bool>(command.isChecked);
}

bool IsCommandChecked(const Command& command)
{
    return command.isChecked && command.isChecked();
}

bool CommandRegistry::Register(Command command)
{
    if (command.id.empty() || m_indexById.contains(command.id))
    {
        return false;
    }

    std::vector<std::string_view> segments;
    if (!command.menuPath.empty())
    {
        segments = SplitMenuPath(command.menuPath);
        if (segments.size() < 2 || segments.size() > kMaxMenuDepth + 1 || HasEmptySegment(segments))
        {
            return false;
        }
    }

    const std::size_t index = m_commands.size();
    if (!segments.empty())
    {
        CommandMenuNode* menu = &m_menuRoot;
        for (std::size_t segmentIndex = 0; segmentIndex + 1 < segments.size(); ++segmentIndex)
        {
            menu = &FindOrAddMenu(*menu, segments[segmentIndex]);
        }

        CommandMenuNode& item = menu->children.emplace_back();
        item.kind = CommandMenuNode::Kind::Item;
        item.label = std::string(segments.back());
        item.commandIndex = index;
    }

    m_indexById.emplace(command.id, index);
    m_commands.push_back(std::move(command));
    return true;
}

bool CommandRegistry::AddSeparator(std::string_view menuPath)
{
    const std::vector<std::string_view> segments = SplitMenuPath(menuPath);
    if (segments.size() > kMaxMenuDepth || HasEmptySegment(segments))
    {
        return false;
    }

    CommandMenuNode* menu = &m_menuRoot;
    for (std::string_view segment : segments)
    {
        menu = &FindOrAddMenu(*menu, segment);
    }
    menu->children.emplace_back().kind = CommandMenuNode::Kind::Separator;
    return true;
}

const Command* CommandRegistry::Find(std::string_view id) const
{
    const auto found = m_indexById.find(std::string(id));
    return found == m_indexById.end() ? nullptr : &m_commands[found->second];
}

bool CommandRegistry::Execute(std::string_view id) const
{
    const Command* command = Find(id);
    return command != nullptr && Execute(*command);
}

bool CommandRegistry::Execute(const Command& command) const
{
    if (!IsCommandEnabled(command))
    {
        return false;
    }
    if (command.execute)
    {
        command.execute();
    }
    return true;
}

std::string FormatShortcut(ImGuiKeyChord shortcut)
{
    if (shortcut == 0)
    {
        return {};
    }
    return ImGui::GetKeyChordName(shortcut);
}
}
