#include "input_monitor_panel.h"

#include <engine/core/log/log.h>

#include <IconsPhosphor.h>
#include <imgui.h>

namespace me
{

InputMonitorPanel::InputMonitorPanel()
    : EditorPanel("input_monitor", "Input Monitor", ICON_PH_KEYBOARD)
{
}

void InputMonitorPanel::PreBegin(EditorContext& context)
{
    static_cast<void>(context);
    ImGui::SetNextWindowSize(ImVec2(720.0f * UiScale(), 360.0f * UiScale()), ImGuiCond_FirstUseEver);
}

void InputMonitorPanel::OnGui(EditorContext& context)
{
    static_cast<void>(context);
    Log::RefreshInputMessagesSnapshot(m_messages, m_messagesRevision);
    const std::vector<std::string>& inputMessages = m_messages;
    ImGui::Text("Captured Events: %u", static_cast<unsigned int>(inputMessages.size()));
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
    {
        Log::ClearInputMessages();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_autoScroll);
    ImGui::Separator();

    if (ImGui::BeginChild("InputMonitorLog", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_HorizontalScrollbar))
    {
        const bool shouldAutoScroll =
            m_autoScroll &&
            ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
        // Only the visible lines are submitted.
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(inputMessages.size()));
        while (clipper.Step())
        {
            for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line)
            {
                const std::string& message = inputMessages[static_cast<size_t>(line)];
                ImGui::TextUnformatted(message.data(), message.data() + message.size());
            }
        }

        if (shouldAutoScroll)
        {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
}
}
