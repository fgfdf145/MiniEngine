#pragma once

#include <engine/asset/asset_manager.h>
#include <engine/asset/kn5_importer.h>
#include <engine/editor/ui/framework/editor_panel.h>

#include <deque>
#include <optional>
#include <string>

namespace me
{

// The project's assets folder: browse, import, copy, rename, delete and drag assets into the scene.
// Models are imported into the folder it shows; an import that needs a choice first asks through
// the Kn5ImportModal or the ImportConflictModal.
class AssetBrowserPanel final : public EditorPanel
{
  public:
    AssetBrowserPanel();

    // Saved as "asset_manager" before every window was.
    std::string GetSettingsKey() const override
    {
        return "asset_manager";
    }

    // The browser, created on first use at the project's assets root.
    AssetManager& Assets();
    // Rescans the folder on the next draw.
    void Refresh();
    // A file or folder dropped onto the editor window from the OS. It is imported (models) or copied
    // into the folder the browser shows, one per frame, once no import is waiting on a choice.
    void QueueDroppedFile(std::string path);
    // Imports a model into the folder being browsed, asking first when its target folder is taken.
    // A .kn5 first asks for its livery and options unless `kn5Options` already holds them.
    void RequestModelImport(
        EditorContext& context,
        const std::string& sourcePath,
        std::optional<Kn5ImportOptions> kn5Options = std::nullopt);

  protected:
    void OnGui(EditorContext& context) override;

  private:
    std::optional<AssetManager> m_assetManager;
    std::deque<std::string> m_droppedFiles;
};
}
