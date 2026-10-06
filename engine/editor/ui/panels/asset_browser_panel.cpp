#include "asset_browser_panel.h"

#include <engine/asset/asset_paths.h>
#include <engine/asset/model_import_target.h>
#include <engine/asset/model_loader.h>
#include <engine/core/log/log.h>
#include <engine/core/paths/engine_paths.h>
#include <engine/editor/editor_ui.h>
#include <engine/editor/ui/editor_ui_internal.h>
#include <engine/editor/ui/framework/editor_window_manager.h>
#include <engine/editor/ui/modals/import_conflict_modal.h>
#include <engine/editor/ui/modals/kn5_import_modal.h>
#include <engine/editor/ui/windows/model_processor_window.h>

#include <IconsPhosphor.h>
#include <imgui.h>

#include <filesystem>
#include <utility>

namespace me
{

AssetBrowserPanel::AssetBrowserPanel()
    : EditorPanel("assets", "Assets", ICON_PH_TREE_VIEW)
{
}

AssetManager& AssetBrowserPanel::Assets()
{
    if (!m_assetManager.has_value())
    {
        m_assetManager.emplace(EnginePaths::AssetsRoot());
    }
    return *m_assetManager;
}

void AssetBrowserPanel::Refresh()
{
    if (m_assetManager.has_value())
    {
        m_assetManager->Refresh();
    }
}

void AssetBrowserPanel::OnGui(EditorContext& context)
{
    EditorUiFrameResult& result = context.result;
    const AssetManagerResult assetResult = Assets().Draw();

    if (const std::optional<std::string> sourcePath =
            PickFilePath(FileDialogType::OpenModel, assetResult.wantsImportModel);
        sourcePath.has_value())
    {
        RequestModelImport(context, *sourcePath);
    }

    // Files dropped onto the window go into the folder being browsed, one per frame: a frame
    // carries one import or copy request, and a model whose folder is taken waits for the
    // conflict modal to be answered.
    if (!m_droppedFiles.empty() && !context.windows.Get<ImportConflictModal>().IsPending() &&
        !context.windows.Get<Kn5ImportModal>().IsPending() &&
        !result.actions.importedModelRequest.has_value() && !assetResult.pasteRequest.has_value())
    {
        const std::string dropped = std::move(m_droppedFiles.front());
        m_droppedFiles.pop_front();
        if (IsImportableModelAssetPath(dropped))
        {
            RequestModelImport(context, dropped);
        }
        else if (!AssetPaths::IsSameOrInside(dropped, Assets().GetAssetsRoot()))
        {
            result.actions.pastedAsset = EditorUiActions::AssetPasteRequest{
                dropped,
                Assets().GetCurrentDirectory().string()};
            Assets().Refresh();
        }
        else
        {
            LOG_INFO("Ignored dropped file '{}': it is already in the assets folder", dropped);
        }
    }
    if (assetResult.openScenePath.has_value())
    {
        result.actions.selectedSceneLoadPath = assetResult.openScenePath;
    }
    if (assetResult.previewAudioPath.has_value())
    {
        result.actions.previewAudioPath = assetResult.previewAudioPath;
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
        Assets().Refresh();
    }
    if (assetResult.pasteRequest.has_value())
    {
        result.actions.pastedAsset = EditorUiActions::AssetPasteRequest{
            assetResult.pasteRequest->sourcePath,
            assetResult.pasteRequest->destinationDirectory};
        Assets().Refresh();
    }
    for (const AssetManagerResult::RenamedAsset& renamed : assetResult.renamedAssets)
    {
        // The model processor saves material edits next to the model it
        // opened; follow the rename so they land beside the renamed file.
        context.windows.Get<ModelProcessorWindow>().OnAssetRenamed(renamed.oldPath, renamed.newPath);
        result.actions.renamedAssets.push_back(renamed);
    }
}

void AssetBrowserPanel::QueueDroppedFile(std::string path)
{
    m_droppedFiles.push_back(std::move(path));
    // Show where the file lands.
    Open();
}

void AssetBrowserPanel::RequestModelImport(
    EditorContext& context,
    const std::string& sourcePath,
    std::optional<Kn5ImportOptions> kn5Options)
{
    const std::string destination = Assets().GetCurrentDirectory().string();
    if ((Kn5Importer::IsKn5Path(sourcePath) || Kn5Importer::IsLayoutPath(sourcePath)) && !kn5Options.has_value())
    {
        context.windows.Get<Kn5ImportModal>().Ask(sourcePath, destination);
        return;
    }

    const std::filesystem::path modelFolder =
        ModelImportTarget::DefaultFolder(ModelLoader::ImportName(sourcePath), destination);
    if (ModelImportTarget::IsOccupied(modelFolder))
    {
        // Same-named models are common (every Sketchfab download is
        // "scene.gltf"): ask rather than silently reuse the old one.
        context.windows.Get<ImportConflictModal>().Ask(ImportConflictModal::Conflict{
            sourcePath,
            destination,
            modelFolder.filename().string(),
            ModelImportTarget::NextFreeFolder(modelFolder).filename().string(),
            kn5Options.value_or(Kn5ImportOptions{})});
    }
    else
    {
        // The import runs on a background thread; the backend calls
        // the browser (RequestAssetBrowserRefresh) once the files are on disk.
        context.result.actions.importedModelRequest = EditorUiActions::ImportedModelRequest{
            sourcePath,
            destination,
            ImportConflictPolicy::FailIfExists,
            kn5Options.value_or(Kn5ImportOptions{})};
    }
}
}
