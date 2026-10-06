#include "import_conflict_modal.h"

#include <engine/editor/editor_ui.h>
#include <engine/editor/ui_colors.h>

#include <imgui.h>

#include <utility>

namespace me
{

ImportConflictModal::ImportConflictModal()
    : EditorModal("import_conflict", "Model Already Imported")
{
}

void ImportConflictModal::Ask(Conflict conflict)
{
    m_pending = std::move(conflict);
    Open();
}

void ImportConflictModal::OnClose(EditorContext& context)
{
    static_cast<void>(context);
    m_pending.reset();
}

void ImportConflictModal::OnGui(EditorContext& context)
{
    if (!m_pending.has_value())
    {
        return;
    }
    const Conflict& conflict = *m_pending;
    ImGui::Text("The folder '%s' already holds files.", conflict.existingFolderName.c_str());
    ImGui::Spacing();
    ImGui::TextDisabled("Keep Both imports into '%s' and leaves the existing model alone.",
                        conflict.keepBothFolderName.c_str());
    ImGui::TextColored(
        ui_colors::kTextWarning,
        "Overwrite deletes everything in '%s', including material edits.",
        conflict.existingFolderName.c_str());
    ImGui::TextDisabled("Scenes that use the replaced model keep referencing it.");
    ImGui::Separator();

    const auto request = [&](ImportConflictPolicy policy)
    {
        // The import runs on a background thread; the backend refreshes the asset browser once the
        // files are on disk.
        context.result.actions.importedModelRequest = EditorUiActions::ImportedModelRequest{
            conflict.sourcePath,
            conflict.destinationDirectory,
            policy,
            conflict.kn5Options};
    };

    const std::string keepBothLabel = "Import as '" + conflict.keepBothFolderName + "'";
    if (ImGui::Button(keepBothLabel.c_str()))
    {
        request(ImportConflictPolicy::KeepBoth);
        m_pending.reset();
        CloseModal();
        return;
    }
    ImGui::SetItemDefaultFocus();

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ui_colors::kFillDanger);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui_colors::kFillDangerHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui_colors::kFillDanger);
    const bool overwrite = ImGui::Button("Overwrite", ImVec2(120.0f * UiScale(), 0.0f));
    ImGui::PopStyleColor(3);
    if (overwrite)
    {
        request(ImportConflictPolicy::Overwrite);
        m_pending.reset();
        CloseModal();
        return;
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f * UiScale(), 0.0f)))
    {
        m_pending.reset();
        CloseModal();
    }
}
}
