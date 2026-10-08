#include "kn5_import_modal.h"

#include <engine/asset/model_import_target.h>
#include <engine/asset/model_loader.h>
#include <engine/editor/ui/framework/editor_context.h>
#include <engine/editor/ui/framework/editor_window_manager.h>
#include <engine/editor/ui/panels/asset_browser_panel.h>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <utility>

namespace me
{

namespace
{
// The track layouts a kn5 belongs to, as radio buttons after "This file only" (0).
void DrawKn5LayoutChoice(const Kn5ModelSummary& summary, size_t& selectedLayout)
{
    // A track: the game draws the layout, of which this file is often only the main part.
    ImGui::SeparatorText("Track Layout");
    if (ImGui::RadioButton("This file only", selectedLayout == 0))
    {
        selectedLayout = 0;
    }
    for (size_t index = 0; index < summary.layouts.size(); ++index)
    {
        const Kn5ModelSummary::Layout& layout = summary.layouts[index];
        const std::string label =
            layout.path.filename().string() + " (" + std::to_string(layout.models) + " models)##layout" + std::to_string(index);
        if (ImGui::RadioButton(label.c_str(), selectedLayout == index + 1))
        {
            selectedLayout = index + 1;
        }
    }
    ImGui::TextDisabled("A layout places every model the game draws for it; the counts above are this file's.");
}

// The liveries beside a car, each with its bodywork's paint swatch, then the kn5's own textures;
// with a note when the chosen one cannot change the body's colour.
void DrawKn5LiveryList(const Kn5ModelSummary& summary, size_t& selectedSkin, float uiScale)
{
    const float rowHeight = ImGui::GetFrameHeightWithSpacing();
    const float listHeight = rowHeight * static_cast<float>(std::min<size_t>(summary.skins.size(), 8)) +
                             ImGui::GetStyle().WindowPadding.y * 2.0f;
    if (ImGui::BeginChild("Liveries", ImVec2(420.0f * uiScale, listHeight), true))
    {
        const float swatchSize = ImGui::GetFrameHeight();
        for (size_t index = 0; index < summary.skins.size(); ++index)
        {
            const Kn5SkinSummary& skin = summary.skins[index];
            ImGui::PushID(static_cast<int>(index));
            if (skin.paint.has_value())
            {
                const ImVec4 paint(
                    (*skin.paint)[0] / 255.0f,
                    (*skin.paint)[1] / 255.0f,
                    (*skin.paint)[2] / 255.0f,
                    1.0f);
                ImGui::ColorButton(
                    "##paint",
                    paint,
                    ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                    ImVec2(swatchSize, swatchSize));
            }
            else
            {
                ImGui::Dummy(ImVec2(swatchSize, swatchSize));
            }
            ImGui::SameLine();

            std::string label = skin.name.empty() ? std::string("Embedded textures (kn5)") : skin.name;
            if (index == 0)
            {
                label += "  (default)";
            }
            ImGui::AlignTextToFramePadding();
            if (ImGui::Selectable(label.c_str(), selectedSkin == index, ImGuiSelectableFlags_None, ImVec2(0.0f, swatchSize)))
            {
                selectedSkin = index;
            }
            if (skin.paint.has_value())
            {
                ImGui::SetItemTooltip(
                    "Paint #%02X%02X%02X from material '%s' (%s)",
                    (*skin.paint)[0],
                    (*skin.paint)[1],
                    (*skin.paint)[2],
                    skin.paintMaterial.c_str(),
                    skin.paintFromSkin ? "this livery" : "the kn5");
            }
            else
            {
                ImGui::SetItemTooltip("The paint is a pattern, not one colour.");
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    const Kn5SkinSummary& chosen = summary.skins[selectedSkin];
    if (!chosen.name.empty() && chosen.paint.has_value() && !chosen.paintFromSkin)
    {
        // Otherwise "I changed the livery and the body did not change" looks like a bug.
        ImGui::TextDisabled("This livery does not ship the paint texture: the body keeps the kn5's colour.");
    }
    else if (chosen.name.empty())
    {
        ImGui::TextDisabled("The kn5's own textures are the export-time template, usually grey primer.");
    }
}
}

Kn5ImportModal::Kn5ImportModal()
    : EditorModal("kn5_import", "Import Assetto Corsa Model")
{
}

void Kn5ImportModal::Ask(const std::string& sourcePath, const std::string& destinationDirectory)
{
    // The livery, or the track layout, lives beside the model, not in it: there is a choice to
    // make first.
    Cancel();
    PendingImport pending;
    pending.sourcePath = sourcePath;
    pending.destinationDirectory = destinationDirectory;
    pending.survey = RunAsync(TaskPriority::Low, [sourcePath]()
                              {
                                  return Kn5Importer::Inspect(sourcePath);
                              });
    m_pending = std::move(pending);
    Open();
}

void Kn5ImportModal::Cancel()
{
    if (m_pending.has_value() && m_pending->survey.valid())
    {
        m_abandonedSurveys.push_back(std::move(m_pending->survey));
    }
    m_pending.reset();
}

void Kn5ImportModal::Tick(EditorContext& context)
{
    static_cast<void>(context);
    m_abandonedSurveys.erase(
        std::remove_if(m_abandonedSurveys.begin(), m_abandonedSurveys.end(), [](const TaskFuture<Kn5ModelSummary>& survey)
                       {
                           return survey.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                       }),
        m_abandonedSurveys.end());
}

void Kn5ImportModal::OnClose(EditorContext& context)
{
    static_cast<void>(context);
    Cancel();
}

void Kn5ImportModal::OnGui(EditorContext& context)
{
    const ImVec4 kWarningColor(1.00f, 0.55f, 0.35f, 1.0f);
    if (!m_pending.has_value())
    {
        CloseModal();
        return;
    }

    PendingImport& pending = *m_pending;
    if (pending.survey.valid() && pending.survey.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        try
        {
            pending.summary = pending.survey.get();
        }
        catch (const std::exception& error)
        {
            pending.error = error.what();
        }
    }

    // What the import converts: the picked file, or one of the track layouts that place it.
    const bool layoutChosen = pending.summary.has_value() && pending.selectedLayout > 0 &&
                              pending.selectedLayout <= pending.summary->layouts.size();
    const std::filesystem::path source =
        layoutChosen ? pending.summary->layouts[pending.selectedLayout - 1].path : std::filesystem::path(pending.sourcePath);
    const bool importsLayout = Kn5Importer::IsLayoutPath(source);
    ImGui::Text("%s", std::filesystem::path(pending.sourcePath).filename().string().c_str());
    ImGui::TextDisabled(
        "Converted to glTF into '%s'.",
        ModelImportTarget::DefaultFolder(ModelLoader::ImportName(source), pending.destinationDirectory).filename().string().c_str());

    bool canImport = false;
    if (!pending.error.empty())
    {
        ImGui::Spacing();
        ImGui::TextColored(kWarningColor, "This file cannot be read: %s", pending.error.c_str());
    }
    else if (!pending.summary.has_value())
    {
        ImGui::Spacing();
        ImGui::TextDisabled("Reading the model...");
    }
    else if (pending.summary->encrypted)
    {
        ImGui::Spacing();
        ImGui::TextColored(kWarningColor, "This file carries the Custom Shaders Patch encryption trailer.");
        ImGui::TextDisabled("Its textures and several meshes are decoys, so it cannot be imported.");
    }
    else
    {
        const Kn5ModelSummary& summary = *pending.summary;
        canImport = true;

        ImGui::SeparatorText("Model");
        if (summary.models > 1)
        {
            ImGui::TextDisabled("A track layout of %zu models.", summary.models);
        }
        ImGui::TextDisabled(
            "%zu meshes, %zu triangles, %zu materials, %zu textures",
            summary.meshes,
            summary.triangles,
            summary.materials,
            summary.textures);
        if (summary.hiddenMeshes > 0)
        {
            ImGui::TextDisabled("%zu meshes the game never draws (physics surfaces, spawn and timing markers) are left out.", summary.hiddenMeshes);
        }

        if (!summary.layouts.empty())
        {
            DrawKn5LayoutChoice(summary, pending.selectedLayout);
        }

        // The last entry is the kn5's own textures; any before it are the skins/ folders.
        const bool hasSkins = summary.skins.size() > 1;
        pending.selectedSkin = std::min(pending.selectedSkin, summary.skins.size() - 1);
        if (importsLayout)
        {
            // Tracks have no liveries.
        }
        else if (!hasSkins)
        {
            ImGui::SeparatorText("Livery");
            ImGui::TextDisabled("No skins folder beside this model: its embedded textures are used.");
        }
        else
        {
            ImGui::SeparatorText("Livery");
            DrawKn5LiveryList(summary, pending.selectedSkin, UiScale());
        }

        ImGui::SeparatorText("Options");
        ImGui::Checkbox("Keep runtime variants", &pending.options.keepVariants);
        ImGui::TextDisabled(
            "%zu *_BLUR, *_DAMAGE and low-res LOD meshes. Kept, they overlap what they replace.",
            summary.runtimeVariants);
        ImGui::Checkbox("Flip V texture coordinate", &pending.options.flipUv);
        ImGui::TextDisabled("Only for mods whose textures arrive upside down.");
    }

    ImGui::Separator();
    ImGui::BeginDisabled(!canImport);
    const bool importClicked = ImGui::Button("Import", ImVec2(120.0f * UiScale(), 0.0f));
    ImGui::EndDisabled();
    if (canImport)
    {
        ImGui::SetItemDefaultFocus();
    }
    ImGui::SameLine();
    const bool cancelled = ImGui::Button("Cancel", ImVec2(120.0f * UiScale(), 0.0f));

    if (importClicked && canImport)
    {
        const Kn5SkinSummary& chosen = pending.summary->skins[pending.selectedSkin];
        Kn5ImportOptions options = pending.options;
        options.skin = chosen.name.empty() ? std::string("none") : chosen.name;
        const std::string sourcePath = source.string();
        m_pending.reset();
        CloseModal();
        // May still ask about a taken folder, with these options carried along.
        context.windows.Get<AssetBrowserPanel>().RequestModelImport(context, sourcePath, options);
    }
    else if (cancelled)
    {
        Cancel();
        CloseModal();
    }
}
}
