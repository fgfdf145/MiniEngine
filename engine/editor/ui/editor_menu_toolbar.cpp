#include "editor_menu_toolbar.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>

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

std::optional<int> FuzzyMatchScore(std::string_view query, std::string_view text)
{
    constexpr int kMatchScore = 1;
    constexpr int kConsecutiveBonus = 5;
    constexpr int kWordStartBonus = 8;
    constexpr int kMaxGapPenalty = 3;
    const auto lower = [](char character)
    {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    };
    const auto isWordStart = [&text](std::size_t index)
    {
        if (index == 0)
        {
            return true;
        }
        const char previous = text[index - 1];
        return previous == ' ' || previous == '/' || previous == '.' || previous == '_' || previous == '-';
    };

    int score = 0;
    std::size_t next = 0;
    std::optional<std::size_t> previousMatch;
    for (const char queryCharacter : query)
    {
        // Spaces only separate words: "save as" finds "Save Scene As".
        if (queryCharacter == ' ')
        {
            continue;
        }
        const char wanted = lower(queryCharacter);
        while (next < text.size() && lower(text[next]) != wanted)
        {
            ++next;
        }
        if (next == text.size())
        {
            return std::nullopt;
        }
        score += kMatchScore;
        if (previousMatch.has_value() && *previousMatch + 1 == next)
        {
            score += kConsecutiveBonus;
        }
        else if (previousMatch.has_value())
        {
            score -= std::min(static_cast<int>(next - *previousMatch - 1), kMaxGapPenalty);
        }
        if (isWordStart(next))
        {
            score += kWordStartBonus;
        }
        previousMatch = next;
        ++next;
    }
    return score;
}

std::vector<std::size_t> FindPaletteCommands(const CommandRegistry& registry, std::string_view query)
{
    // A label match beats the same match in the menu path, which also holds the menu's name.
    constexpr int kMenuPathPenalty = 2;
    std::vector<std::pair<int, std::size_t>> scored;
    const std::vector<Command>& commands = registry.GetCommands();
    for (std::size_t index = 0; index < commands.size(); ++index)
    {
        const Command& command = commands[index];
        if (!IsCommandEnabled(command))
        {
            continue;
        }
        std::optional<int> score = FuzzyMatchScore(query, command.label);
        if (const std::optional<int> pathScore = FuzzyMatchScore(query, command.menuPath); pathScore.has_value())
        {
            score = std::max(score.value_or(*pathScore - kMenuPathPenalty), *pathScore - kMenuPathPenalty);
        }
        if (score.has_value())
        {
            scored.emplace_back(*score, index);
        }
    }
    // Equal scores keep registration order, which is menu order.
    std::stable_sort(scored.begin(), scored.end(), [](const auto& left, const auto& right)
                     {
                         return left.first > right.first;
                     });
    std::vector<std::size_t> indices;
    indices.reserve(scored.size());
    for (const auto& [score, index] : scored)
    {
        indices.push_back(index);
    }
    return indices;
}

void CommandPalette::Draw(const CommandRegistry& registry, float uiScale)
{
    constexpr const char* kPopupId = "##CommandPalette";
    constexpr std::size_t kVisibleRows = 12;
    if (m_openRequested)
    {
        m_openRequested = false;
        m_query.clear();
        m_selected = 0;
        m_focusInput = true;
        ImGui::OpenPopup(kPopupId);
    }

    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    const float width = std::min(560.0f * uiScale, mainViewport->WorkSize.x - 32.0f * uiScale);
    ImGui::SetNextWindowPos(
        ImVec2(mainViewport->WorkPos.x + mainViewport->WorkSize.x * 0.5f, mainViewport->WorkPos.y + 48.0f * uiScale),
        ImGuiCond_Always,
        ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    if (!ImGui::BeginPopup(kPopupId, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings))
    {
        return;
    }

    if (m_focusInput)
    {
        ImGui::SetKeyboardFocusHere();
        m_focusInput = false;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    const std::string previousQuery = m_query;
    const bool enterPressed =
        ImGui::InputTextWithHint("##Query", "Type a command", &m_query, ImGuiInputTextFlags_EnterReturnsTrue);
    if (m_query != previousQuery)
    {
        m_selected = 0;
    }

    const std::vector<std::size_t> matches = FindPaletteCommands(registry, m_query);
    bool navigated = false;
    if (!matches.empty())
    {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
        {
            m_selected = (m_selected + 1) % matches.size();
            navigated = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
        {
            m_selected = (m_selected + matches.size() - 1) % matches.size();
            navigated = true;
        }
        m_selected = std::min(m_selected, matches.size() - 1);
    }

    const Command* chosen = nullptr;
    if (matches.empty())
    {
        ImGui::TextDisabled("No enabled command matches.");
    }
    else
    {
        const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
        const float listHeight = rowHeight * static_cast<float>(std::min(matches.size(), kVisibleRows)) + ImGui::GetStyle().WindowPadding.y;
        if (ImGui::BeginChild("##Matches", ImVec2(0.0f, listHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav))
        {
            const std::vector<Command>& commands = registry.GetCommands();
            for (std::size_t row = 0; row < matches.size(); ++row)
            {
                const Command& command = commands[matches[row]];
                ImGui::PushID(static_cast<int>(matches[row]));
                const std::string text = command.icon.empty() ? command.label : command.icon + "  " + command.label;
                const bool selected = row == m_selected;
                if (ImGui::Selectable(text.c_str(), selected))
                {
                    chosen = &command;
                }
                if (selected && navigated)
                {
                    ImGui::SetScrollHereY();
                }
                // Where it lives and how to reach it without the palette, right-aligned.
                std::string detail = command.menuPath;
                if (const std::string shortcut = FormatShortcut(command.shortcut); !shortcut.empty())
                {
                    detail = detail.empty() ? shortcut : detail + "   " + shortcut;
                }
                if (!detail.empty())
                {
                    const float detailWidth = ImGui::CalcTextSize(detail.c_str()).x;
                    ImGui::SameLine(std::max(ImGui::GetContentRegionMax().x - detailWidth, ImGui::GetCursorPosX()));
                    ImGui::TextDisabled("%s", detail.c_str());
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        if (enterPressed)
        {
            chosen = &registry.GetCommands()[matches[m_selected]];
        }
    }

    if (chosen != nullptr || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    if (chosen != nullptr)
    {
        registry.Execute(*chosen);
    }
}

void DrawKeyboardShortcutsWindow(
    const CommandRegistry& registry,
    bool* open,
    std::span<const std::pair<const char*, const char*>> extraKeys)
{
    if (!ImGui::Begin("Keyboard Shortcuts", open))
    {
        ImGui::End();
        return;
    }

    // One table per top-level menu, in menu order; toolbar-only commands under "Toolbar".
    std::vector<std::string> groups;
    const auto groupOf = [](const Command& command)
    {
        const std::size_t slash = command.menuPath.find('/');
        return slash == std::string::npos ? std::string("Toolbar") : command.menuPath.substr(0, slash);
    };
    for (const Command& command : registry.GetCommands())
    {
        if (command.shortcut != 0 && std::find(groups.begin(), groups.end(), groupOf(command)) == groups.end())
        {
            groups.push_back(groupOf(command));
        }
    }

    constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
    const auto drawRow = [](const char* keys, const char* action, bool enabled)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::BeginDisabled(!enabled);
        ImGui::TextUnformatted(keys);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(action);
        ImGui::EndDisabled();
    };
    for (const std::string& group : groups)
    {
        ImGui::SeparatorText(group.c_str());
        if (ImGui::BeginTable(group.c_str(), 2, kTableFlags))
        {
            for (const Command& command : registry.GetCommands())
            {
                if (command.shortcut != 0 && groupOf(command) == group)
                {
                    // Disabled commands are listed greyed out: their keys do nothing yet.
                    drawRow(FormatShortcut(command.shortcut).c_str(), command.label.c_str(), IsCommandEnabled(command));
                }
            }
            ImGui::EndTable();
        }
    }
    if (!extraKeys.empty())
    {
        ImGui::SeparatorText("Viewport");
        if (ImGui::BeginTable("##ExtraKeys", 2, kTableFlags))
        {
            for (const auto& [keys, action] : extraKeys)
            {
                drawRow(keys, action, true);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}
}
