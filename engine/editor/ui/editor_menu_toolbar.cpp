#include "editor_menu_toolbar.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>

namespace me
{

namespace
{
constexpr float kToolbarPaddingX = 8.0f;
constexpr float kToolbarPaddingY = 4.0f;
constexpr float kToolbarItemSpacing = 2.0f;   // between the items of one group
constexpr float kToolbarGroupSpacing = 16.0f; // between groups
constexpr ImGuiKeyChord kNonTypingModifiers = ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiMod_Super;

struct ToolbarMetrics
{
    float buttonSize = 0.0f; // icon buttons are square
    float itemSpacing = 0.0f;
    float groupSpacing = 0.0f;
    float uiScale = 1.0f;
};

void DrawMenuChildren(const CommandRegistry& registry, const std::vector<CommandMenuNode>& children)
{
    for (const CommandMenuNode& node : children)
    {
        switch (node.kind)
        {
        case CommandMenuNode::Kind::Separator:
            ImGui::Separator();
            break;
        case CommandMenuNode::Kind::Menu:
            if (ImGui::BeginMenu(node.label.c_str()))
            {
                DrawMenuChildren(registry, node.children);
                ImGui::EndMenu();
            }
            break;
        case CommandMenuNode::Kind::Item:
        {
            const Command& command = registry.GetCommands()[node.commandIndex];
            // Display only: the shortcut itself is handled by ProcessCommandShortcuts.
            const std::string shortcut = FormatShortcut(command.shortcut);
            ImGui::PushID(static_cast<int>(node.commandIndex));
            if (ImGui::MenuItemEx(
                    node.label.c_str(),
                    command.icon.empty() ? nullptr : command.icon.c_str(),
                    shortcut.empty() ? nullptr : shortcut.c_str(),
                    IsCommandChecked(command),
                    IsCommandEnabled(command)))
            {
                registry.Execute(command);
            }
            ImGui::PopID();
            break;
        }
        }
    }
}

// "Reload Shaders (Ctrl+R)", or just the label when there is no shortcut.
void SetCommandTooltip(const Command& command)
{
    const std::string shortcut = FormatShortcut(command.shortcut);
    if (shortcut.empty())
    {
        ImGui::SetItemTooltip("%s", command.label.c_str());
    }
    else
    {
        ImGui::SetItemTooltip("%s (%s)", command.label.c_str(), shortcut.c_str());
    }
}

float MeasureItem(const CommandRegistry& registry, const ToolbarItem& item, const ToolbarMetrics& metrics)
{
    if (!item.dropdownCommandIds.empty())
    {
        return item.dropdownWidth * metrics.uiScale;
    }
    const Command* command = registry.Find(item.commandId);
    if (command == nullptr)
    {
        return 0.0f;
    }
    if (!command->icon.empty())
    {
        return metrics.buttonSize;
    }
    return ImGui::CalcTextSize(command->label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

float MeasureSection(const CommandRegistry& registry, const std::vector<ToolbarGroup>& groups, const ToolbarMetrics& metrics)
{
    float width = 0.0f;
    bool first = true;
    for (const ToolbarGroup& group : groups)
    {
        for (std::size_t itemIndex = 0; itemIndex < group.size(); ++itemIndex)
        {
            if (!first)
            {
                width += itemIndex == 0 ? metrics.groupSpacing : metrics.itemSpacing;
            }
            width += MeasureItem(registry, group[itemIndex], metrics);
            first = false;
        }
    }
    return width;
}

void DrawCommandButton(const CommandRegistry& registry, const Command& command, const ToolbarMetrics& metrics)
{
    const bool checked = IsCommandChecked(command);
    ImGui::PushID(command.id.c_str());
    if (checked)
    {
        const ImVec4 activeColor = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Button, activeColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeColor);
    }

    ImGui::BeginDisabled(!IsCommandEnabled(command));
    const bool hasIcon = !command.icon.empty();
    const bool pressed = ImGui::Button(
        hasIcon ? command.icon.c_str() : command.label.c_str(),
        ImVec2(hasIcon ? metrics.buttonSize : 0.0f, metrics.buttonSize));
    ImGui::EndDisabled();
    SetCommandTooltip(command);

    if (checked)
    {
        ImGui::PopStyleColor(2);
    }
    if (pressed)
    {
        registry.Execute(command);
    }
    ImGui::PopID();
}

void DrawCommandDropdown(const CommandRegistry& registry, const ToolbarItem& item, const ToolbarMetrics& metrics)
{
    const Command* current = nullptr;
    for (const std::string& commandId : item.dropdownCommandIds)
    {
        const Command* command = registry.Find(commandId);
        if (command != nullptr && IsCommandChecked(*command))
        {
            current = command;
            break;
        }
    }

    std::string preview = item.dropdownTooltip;
    if (current != nullptr)
    {
        preview = current->icon.empty() ? current->label : current->icon + "  " + current->label;
    }

    ImGui::PushID(item.dropdownTooltip.c_str());
    ImGui::SetNextItemWidth(item.dropdownWidth * metrics.uiScale);
    const bool open = ImGui::BeginCombo("##Dropdown", preview.c_str());
    if (!open)
    {
        ImGui::SetItemTooltip("%s", item.dropdownTooltip.c_str());
    }
    else
    {
        for (const std::string& commandId : item.dropdownCommandIds)
        {
            const Command* command = registry.Find(commandId);
            if (command == nullptr)
            {
                continue;
            }
            const std::string shortcut = FormatShortcut(command->shortcut);
            const bool checked = IsCommandChecked(*command);
            if (ImGui::MenuItemEx(
                    command->label.c_str(),
                    command->icon.empty() ? nullptr : command->icon.c_str(),
                    shortcut.empty() ? nullptr : shortcut.c_str(),
                    checked,
                    IsCommandEnabled(*command)))
            {
                registry.Execute(*command);
            }
            if (checked)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

// Draws a section with its first item at startX (window coordinates); returns where it ends.
float DrawSection(
    const CommandRegistry& registry,
    const std::vector<ToolbarGroup>& groups,
    float startX,
    bool sameLineAsPrevious,
    const ToolbarMetrics& metrics)
{
    bool first = true;
    for (const ToolbarGroup& group : groups)
    {
        for (std::size_t itemIndex = 0; itemIndex < group.size(); ++itemIndex)
        {
            const ToolbarItem& item = group[itemIndex];
            if (first)
            {
                if (sameLineAsPrevious)
                {
                    ImGui::SameLine(startX);
                }
                else
                {
                    ImGui::SetCursorPosX(startX);
                }
            }
            else
            {
                ImGui::SameLine(0.0f, itemIndex == 0 ? metrics.groupSpacing : metrics.itemSpacing);
            }
            first = false;

            if (!item.dropdownCommandIds.empty())
            {
                DrawCommandDropdown(registry, item, metrics);
            }
            else if (const Command* command = registry.Find(item.commandId); command != nullptr)
            {
                DrawCommandButton(registry, *command, metrics);
            }
        }
    }
    return startX + MeasureSection(registry, groups, metrics);
}
}

void ProcessCommandShortcuts(const CommandRegistry& registry)
{
    // A modal dialog owns the keyboard until it closes.
    if (ImGui::GetTopMostPopupModal() != nullptr)
    {
        return;
    }

    const bool typingText = ImGui::GetIO().WantTextInput;
    for (const Command& command : registry.GetCommands())
    {
        if (command.shortcut == 0 || !IsCommandEnabled(command))
        {
            continue;
        }
        if (typingText && (command.shortcut & kNonTypingModifiers) == 0)
        {
            continue;
        }
        if (ImGui::Shortcut(command.shortcut, ImGuiInputFlags_RouteGlobal))
        {
            registry.Execute(command);
        }
    }
}

void DrawMainMenu(const CommandRegistry& registry)
{
    if (!ImGui::BeginMainMenuBar())
    {
        return;
    }
    for (const CommandMenuNode& menu : registry.GetMenuRoot().children)
    {
        if (menu.kind == CommandMenuNode::Kind::Menu && ImGui::BeginMenu(menu.label.c_str()))
        {
            DrawMenuChildren(registry, menu.children);
            ImGui::EndMenu();
        }
    }
    ImGui::EndMainMenuBar();
}

void DrawToolbar(const CommandRegistry& registry, const ToolbarLayout& layout, float uiScale)
{
    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    if (mainViewport == nullptr)
    {
        return;
    }

    ToolbarMetrics metrics;
    metrics.buttonSize = ImGui::GetFrameHeight();
    metrics.itemSpacing = kToolbarItemSpacing * uiScale;
    metrics.groupSpacing = kToolbarGroupSpacing * uiScale;
    metrics.uiScale = uiScale;

    const ImVec2 padding(kToolbarPaddingX * uiScale, kToolbarPaddingY * uiScale);
    const float height = metrics.buttonSize + padding.y * 2.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    const bool open = ImGui::BeginViewportSideBar(
        "##Toolbar",
        mainViewport,
        ImGuiDir_Up,
        height,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();

    if (open)
    {
        const float windowWidth = ImGui::GetWindowWidth();
        const float leftEnd = DrawSection(registry, layout.left, padding.x, false, metrics);

        const float centerWidth = MeasureSection(registry, layout.center, metrics);
        const float centerStart = std::max((windowWidth - centerWidth) * 0.5f, leftEnd + metrics.groupSpacing);
        const float centerEnd = DrawSection(registry, layout.center, centerStart, !layout.left.empty(), metrics);

        const float rightWidth = MeasureSection(registry, layout.right, metrics);
        const float rightStart = std::max(windowWidth - padding.x - rightWidth, centerEnd + metrics.groupSpacing);
        DrawSection(registry, layout.right, rightStart, !layout.left.empty() || !layout.center.empty(), metrics);
    }
    ImGui::End();
}
}
