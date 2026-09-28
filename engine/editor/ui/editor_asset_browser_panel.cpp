#include <engine/editor/editor_ui.h>
#include "editor_ui_internal.h"

#include <engine/asset/asset_paths.h>

#include <engine/core/log/log.h>
#include <engine/core/paths/engine_paths.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace me
{

void EditorUiController::DrawAssetBrowserPanel(EditorUiFrameResult& result)
{
    if (!m_assetManager.has_value())
    {
        m_assetManager.emplace(EnginePaths::AssetsRoot());
    }
    if (ImGui::Begin("Assets", &m_showAssetManagerWindow))
    {
        const AssetManagerResult assetResult = m_assetManager->Draw();

        if (const std::optional<std::string> sourcePath =
                PickFilePath(FileDialogType::OpenModel, assetResult.wantsImportModel);
            sourcePath.has_value())
        {
            RequestModelImport(*sourcePath, result);
        }

        // Files dropped onto the window go into the folder being browsed, one per frame: a frame
        // carries one import or copy request, and a model whose folder is taken waits for the
        // conflict modal to be answered.
        if (!m_droppedFiles.empty() && !m_pendingImportConflict.has_value() && !m_pendingKn5Import.has_value() &&
            !result.actions.importedModelRequest.has_value() && !assetResult.pasteRequest.has_value())
        {
            const std::string dropped = std::move(m_droppedFiles.front());
            m_droppedFiles.pop_front();
            if (IsImportableModelAssetPath(dropped))
            {
                RequestModelImport(dropped, result);
            }
            else if (!AssetPaths::IsSameOrInside(dropped, m_assetManager->GetAssetsRoot()))
            {
                result.actions.pastedAsset = EditorUiActions::AssetPasteRequest{
                    dropped,
                    m_assetManager->GetCurrentDirectory().string()};
                m_assetManager->Refresh();
            }
            else
            {
                LOG_INFO("Ignored dropped file '{}': it is already in the assets folder", dropped);
            }
        }
        if (assetResult.selectedModelPath.has_value())
        {
            result.actions.selectedModelPath = assetResult.selectedModelPath;
        }
        for (const std::string& path : assetResult.batchLoadModelPaths)
        {
            result.actions.batchLoadModelPaths.push_back(path);
        }
        if (!assetResult.deleteRequests.empty())
        {
            for (const std::string& path : assetResult.deleteRequests)
            {
                result.actions.deleteAssetPaths.push_back(path);
            }
            m_assetManager->Refresh();
        }
        if (assetResult.pasteRequest.has_value())
        {
            result.actions.pastedAsset = EditorUiActions::AssetPasteRequest{
                assetResult.pasteRequest->sourcePath,
                assetResult.pasteRequest->destinationDirectory};
            m_assetManager->Refresh();
        }
        for (const AssetManagerResult::RenamedAsset& renamed : assetResult.renamedAssets)
        {
            // The model processor saves material edits next to the model it
            // opened; follow the rename so they land beside the renamed file.
            if (const std::optional<std::filesystem::path> rebased =
                    AssetPaths::Rebase(m_modelProcessorModelPath, renamed.oldPath, renamed.newPath))
            {
                m_modelProcessorModelPath = rebased->string();
                m_modelProcessorDisplayName = rebased->filename().string();
            }
            result.actions.renamedAssets.push_back(renamed);
        }
        DrawKn5ImportModal(result);
        DrawImportConflictModal(result);
    }
    ImGui::End();
}

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

void EditorUiController::QueueDroppedFile(std::string path)
{
    m_droppedFiles.push_back(std::move(path));
    // Show where the file lands.
    m_showAssetManagerWindow = true;
}

void EditorUiController::RequestModelImport(
    const std::string& sourcePath,
    EditorUiFrameResult& result,
    std::optional<Kn5ImportOptions> kn5Options)
{
    const std::string destination = m_assetManager->GetCurrentDirectory().string();
    if ((Kn5Importer::IsKn5Path(sourcePath) || Kn5Importer::IsLayoutPath(sourcePath)) && !kn5Options.has_value())
    {
        // The livery, or the track layout, lives beside the model, not in it: there is a choice
        // to make first.
        PendingKn5Import pending;
        pending.sourcePath = sourcePath;
        pending.destinationDirectory = destination;
        pending.survey = std::async(std::launch::async, [sourcePath]()
                                    {
                                        return Kn5Importer::Inspect(sourcePath);
                                    });
        m_pendingKn5Import = std::move(pending);
        m_openKn5ImportModal = true;
        return;
    }

    const std::filesystem::path modelFolder =
        ModelImportTarget::DefaultFolder(ModelLoader::ImportName(sourcePath), destination);
    if (ModelImportTarget::IsOccupied(modelFolder))
    {
        // Same-named models are common (every Sketchfab download is
        // "scene.gltf"): ask rather than silently reuse the old one.
        m_pendingImportConflict = PendingImportConflict{
            sourcePath,
            destination,
            modelFolder.filename().string(),
            ModelImportTarget::NextFreeFolder(modelFolder).filename().string(),
            kn5Options.value_or(Kn5ImportOptions{})};
        m_openImportConflictModal = true;
    }
    else
    {
        // The import runs on a background thread; the backend calls
        // RequestAssetBrowserRefresh() once the files are on disk.
        result.actions.importedModelRequest = EditorUiActions::ImportedModelRequest{
            sourcePath,
            destination,
            ImportConflictPolicy::FailIfExists,
            kn5Options.value_or(Kn5ImportOptions{})};
    }
}

void EditorUiController::DrawImportConflictModal(EditorUiFrameResult& result)
{
    constexpr const char* kTitle = "Model Already Imported";

    if (m_openImportConflictModal)
    {
        ImGui::OpenPopup(kTitle);
        m_openImportConflictModal = false;
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (m_pendingImportConflict.has_value())
        {
            const PendingImportConflict& conflict = *m_pendingImportConflict;
            ImGui::Text("The folder '%s' already holds files.", conflict.existingFolderName.c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("Keep Both imports into '%s' and leaves the existing model alone.",
                                conflict.keepBothFolderName.c_str());
            ImGui::TextColored(
                ImVec4(1.00f, 0.55f, 0.35f, 1.0f),
                "Overwrite deletes everything in '%s', including material edits.",
                conflict.existingFolderName.c_str());
            ImGui::TextDisabled("Scenes that use the replaced model keep referencing it.");
            ImGui::Separator();

            const auto request = [&](ImportConflictPolicy policy)
            {
                result.actions.importedModelRequest = EditorUiActions::ImportedModelRequest{
                    conflict.sourcePath,
                    conflict.destinationDirectory,
                    policy,
                    conflict.kn5Options};
            };

            const std::string keepBothLabel = "Import as '" + conflict.keepBothFolderName + "'";
            if (ImGui::Button(keepBothLabel.c_str()))
            {
                request(ImportConflictPolicy::KeepBoth);
                m_pendingImportConflict.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.25f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.30f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.20f, 0.20f, 1.0f));
            const bool overwrite = ImGui::Button("Overwrite", ImVec2(120.0f * m_effectiveUiScale, 0.0f));
            ImGui::PopStyleColor(3);
            if (overwrite)
            {
                request(ImportConflictPolicy::Overwrite);
                m_pendingImportConflict.reset();
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120.0f * m_effectiveUiScale, 0.0f)))
            {
                m_pendingImportConflict.reset();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    else if (m_pendingImportConflict.has_value())
    {
        // Dismissed without an explicit choice (e.g. Escape): treat as cancel.
        m_pendingImportConflict.reset();
    }
}

void EditorUiController::DrawKn5ImportModal(EditorUiFrameResult& result)
{
    constexpr const char* kTitle = "Import Assetto Corsa Model";
    const ImVec4 kWarningColor(1.00f, 0.55f, 0.35f, 1.0f);

    // Surveys of cancelled dialogs are let go once they finish.
    m_abandonedKn5Surveys.erase(
        std::remove_if(m_abandonedKn5Surveys.begin(), m_abandonedKn5Surveys.end(), [](const std::future<Kn5ModelSummary>& survey)
                       {
                           return survey.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                       }),
        m_abandonedKn5Surveys.end());

    if (m_openKn5ImportModal)
    {
        ImGui::OpenPopup(kTitle);
        m_openKn5ImportModal = false;
    }

    const auto cancel = [&]()
    {
        if (m_pendingKn5Import.has_value() && m_pendingKn5Import->survey.valid())
        {
            m_abandonedKn5Surveys.push_back(std::move(m_pendingKn5Import->survey));
        }
        m_pendingKn5Import.reset();
    };

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (m_pendingKn5Import.has_value() && !m_openKn5ImportModal)
        {
            // Dismissed without an explicit choice (e.g. Escape): treat as cancel.
            cancel();
        }
        return;
    }
    if (!m_pendingKn5Import.has_value())
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    PendingKn5Import& pending = *m_pendingKn5Import;
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
            DrawKn5LiveryList(summary, pending.selectedSkin, m_effectiveUiScale);
        }

        ImGui::SeparatorText("Options");
        ImGui::Checkbox("Keep runtime variants", &pending.options.keepVariants);
        ImGui::TextDisabled(
            "%zu *_BLUR, *_DAMAGE and low-res or far LOD meshes. Kept, they overlap what they replace.",
            summary.runtimeVariants);
        ImGui::Checkbox("Flip V texture coordinate", &pending.options.flipUv);
        ImGui::TextDisabled("Only for mods whose textures arrive upside down.");
    }

    ImGui::Separator();
    ImGui::BeginDisabled(!canImport);
    const bool importClicked = ImGui::Button("Import", ImVec2(120.0f * m_effectiveUiScale, 0.0f));
    ImGui::EndDisabled();
    if (canImport)
    {
        ImGui::SetItemDefaultFocus();
    }
    ImGui::SameLine();
    const bool cancelled = ImGui::Button("Cancel", ImVec2(120.0f * m_effectiveUiScale, 0.0f));

    if (importClicked && canImport)
    {
        const Kn5SkinSummary& chosen = pending.summary->skins[pending.selectedSkin];
        Kn5ImportOptions options = pending.options;
        options.skin = chosen.name.empty() ? std::string("none") : chosen.name;
        const std::string sourcePath = source.string();
        m_pendingKn5Import.reset();
        ImGui::CloseCurrentPopup();
        // May still ask about a taken folder, with these options carried along.
        RequestModelImport(sourcePath, result, options);
    }
    else if (cancelled)
    {
        cancel();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
}
