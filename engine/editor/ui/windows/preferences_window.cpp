#include "preferences_window.h"

#include <engine/editor/editor_ui.h>
#include <engine/editor/ui/editor_ui_internal.h>
#include <engine/editor/ui/framework/editor_window_manager.h>
#include <engine/editor/ui/panels/graphics_debug_panel.h>
#include <engine/editor/ui/panels/theme_panel.h>
#include <engine/core/threading/task_system.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <string>

namespace me
{

PreferencesWindow::PreferencesWindow()
    : EditorWindow("preferences", "Preferences")
{
}

void PreferencesWindow::PreBegin(EditorContext& context)
{
    static_cast<void>(context);
    ImGui::SetNextWindowSize(ImVec2(420.0f * UiScale(), 0.0f), ImGuiCond_FirstUseEver);
}

void PreferencesWindow::OnGui(EditorContext& context)
{
    EngineAudioSettings& audio = context.state.audio;
    ImGui::SeparatorText("Interface");
    // The same setting as the Camera panel's, saved with the editor settings.
    if (DragFloatInRange("UI Scale Multiplier", &context.style.UiScaleMultiplier(), 0.75f, 2.50f, "%.2f x"))
    {
        context.style.ApplyUiScale();
    }
    ImGui::Text("Effective UI Scale: %.2f x", context.style.EffectiveUiScale());
    ImGui::SeparatorText("Theme");
    ImGui::TextUnformatted("The editor's colours are edited in the Theme window.");
    if (ImGui::Button("Open Theme"))
    {
        context.windows.Open<ThemePanel>();
    }
    ImGui::SeparatorText("Audio");
    float volumePercent = audio.masterVolume * 100.0f;
    ImGui::BeginDisabled(audio.muted);
    if (DragFloatInRange("Master Volume", &volumePercent, 0.0f, 100.0f, "%.0f %%"))
    {
        audio.masterVolume = volumePercent / 100.0f;
    }
    ImGui::EndDisabled();
    ImGui::Checkbox("Mute", &audio.muted);
    ImGui::TextDisabled("Output: %s", context.state.audioStatus.empty() ? "None" : context.state.audioStatus.c_str());
    DrawProcessSection(context);
    ImGui::SeparatorText("Rendering");
    ImGui::TextUnformatted("Debug views, tone mapping and the render passes' switches are in");
    ImGui::TextUnformatted("the Graphics Debug window.");
    if (ImGui::Button("Open Graphics Debug"))
    {
        context.windows.Open<GraphicsDebugPanel>();
    }
}

void PreferencesWindow::DrawProcessSection(EditorContext& context)
{
    namespace process = platform::process;
    if (!m_topology.has_value())
    {
        m_topology = process::QueryProcessorTopology();
    }
    const process::ProcessorTopology& topology = *m_topology;
    process::ProcessAllocation& allocation = context.state.process;

    ImGui::SeparatorText("Process");
    if (ImGui::BeginCombo("Priority", process::ProcessPriorityLabel(allocation.priority).data()))
    {
        for (const process::ProcessPriority priority :
             {process::ProcessPriority::BelowNormal, process::ProcessPriority::Normal, process::ProcessPriority::AboveNormal,
              process::ProcessPriority::High})
        {
            if (ImGui::Selectable(process::ProcessPriorityLabel(priority).data(), priority == allocation.priority))
            {
                allocation.priority = priority;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The OS priority class of the whole engine. High keeps background programs from taking its CPU time.");

    const std::array<std::string, 3> selectionLabels = {
        "All (" + std::to_string(topology.processors.size()) + ")",
        "Performance Cores (" + std::to_string(topology.PerformanceCount()) + ")",
        "Custom"};
    if (ImGui::BeginCombo("CPUs", selectionLabels[static_cast<size_t>(allocation.cpus)].c_str()))
    {
        for (const process::CpuSelection selection :
             {process::CpuSelection::All, process::CpuSelection::Performance, process::CpuSelection::Custom})
        {
            // Without efficiency cores every CPU is a performance one: the choice would be All again.
            const ImGuiSelectableFlags flags =
                selection == process::CpuSelection::Performance && !topology.IsHybrid() ? ImGuiSelectableFlags_Disabled : 0;
            if (ImGui::Selectable(selectionLabels[static_cast<size_t>(selection)].c_str(), selection == allocation.cpus, flags) &&
                selection != allocation.cpus)
            {
                // Custom starts from the CPUs in use, to take some away or add some.
                if (selection == process::CpuSelection::Custom)
                {
                    allocation.customCpus = process::ResolveCpuSelection(allocation, topology);
                }
                allocation.cpus = selection;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The logical processors the engine's threads may run on, as Task Manager's Set Affinity.");
    if (allocation.cpus == process::CpuSelection::Custom)
    {
        DrawCpuGrid(allocation);
    }

    ImGui::TextDisabled("Now: %s", context.state.processStatus.empty() ? "Unknown" : context.state.processStatus.c_str());
    ImGui::TextDisabled("Task threads: %u, sized for the CPUs given at start-up", TaskSystem::ThreadCount());
}

void PreferencesWindow::DrawCpuGrid(platform::process::ProcessAllocation& allocation)
{
    namespace process = platform::process;
    const process::ProcessorTopology& topology = *m_topology;
    const size_t chosenCount = process::ResolveCpuSelection(allocation, topology).size();

    const float cellWidth = ImGui::GetFrameHeight() + ImGui::CalcTextSize("00").x + ImGui::GetStyle().ItemInnerSpacing.x +
                            ImGui::GetStyle().ItemSpacing.x;
    const int columns = std::clamp(static_cast<int>(ImGui::GetContentRegionAvail().x / cellWidth), 1, 16);
    const bool hybrid = topology.IsHybrid();
    for (const bool performance : {true, false})
    {
        if (!hybrid && !performance)
        {
            break;
        }
        if (hybrid)
        {
            ImGui::TextUnformatted(performance ? "Performance cores" : "Efficiency cores");
        }
        ImGui::PushID(performance ? "performance" : "efficiency");
        if (ImGui::BeginTable("##cpus", columns, ImGuiTableFlags_SizingFixedSame))
        {
            for (const process::LogicalProcessor& processor : topology.processors)
            {
                if (hybrid && topology.IsPerformance(processor) != performance)
                {
                    continue;
                }
                ImGui::TableNextColumn();
                bool chosen = std::binary_search(allocation.customCpus.begin(), allocation.customCpus.end(), processor.index);
                // The last CPU stays: a process needs one to run on.
                ImGui::BeginDisabled(chosen && chosenCount <= 1);
                ImGui::PushID(static_cast<int>(processor.index));
                if (ImGui::Checkbox(std::to_string(processor.index).c_str(), &chosen))
                {
                    if (chosen)
                    {
                        allocation.customCpus.insert(
                            std::upper_bound(allocation.customCpus.begin(), allocation.customCpus.end(), processor.index),
                            processor.index);
                    }
                    else
                    {
                        std::erase(allocation.customCpus, processor.index);
                    }
                }
                ImGui::PopID();
                ImGui::EndDisabled();
                ImGui::SetItemTooltip("CPU %u: core %u", processor.index, processor.core);
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
}
}
