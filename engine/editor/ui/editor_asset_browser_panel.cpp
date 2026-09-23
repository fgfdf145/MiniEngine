#include <engine/editor/editor_ui.h>
#include "editor_ui_internal.h"

#include <engine/asset/asset_paths.h>

#include <engine/core/log/log.h>
#include <engine/core/paths/engine_paths.h>
#include <imgui.h>

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
        if (!m_droppedFiles.empty() && !m_pendingImportConflict.has_value() &&
            !result.actions.importedModelRequest.has_value() && !assetResult.pasteRequest.has_value())
        {
            const std::string dropped = std::move(m_droppedFiles.front());
            m_droppedFiles.pop_front();
            if (IsSupportedModelAssetPath(dropped))
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
        DrawImportConflictModal(result);
    }
    ImGui::End();
}

void EditorUiController::QueueDroppedFile(std::string path)
{
    m_droppedFiles.push_back(std::move(path));
    // Show where the file lands.
    m_showAssetManagerWindow = true;
}

void EditorUiController::RequestModelImport(const std::string& sourcePath, EditorUiFrameResult& result)
{
    const std::string destination = m_assetManager->GetCurrentDirectory().string();
    const std::filesystem::path modelFolder =
        ModelImportTarget::DefaultFolder(std::filesystem::path(sourcePath), destination);
    if (ModelImportTarget::IsOccupied(modelFolder))
    {
        // Same-named models are common (every Sketchfab download is
        // "scene.gltf"): ask rather than silently reuse the old one.
        m_pendingImportConflict = PendingImportConflict{
            sourcePath,
            destination,
            modelFolder.filename().string(),
            ModelImportTarget::NextFreeFolder(modelFolder).filename().string()};
        m_openImportConflictModal = true;
    }
    else
    {
        // The import runs on a background thread; the backend calls
        // RequestAssetBrowserRefresh() once the files are on disk.
        result.actions.importedModelRequest = EditorUiActions::ImportedModelRequest{
            sourcePath,
            destination};
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
                    policy};
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
}
