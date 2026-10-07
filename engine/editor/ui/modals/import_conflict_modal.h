#pragma once

#include <engine/asset/kn5_importer.h>
#include <engine/editor/ui/framework/editor_modal.h>

#include <optional>
#include <string>

namespace me
{

// An import whose model folder already holds files: keep both, overwrite or cancel. Same-named
// models are common (every Sketchfab download is "scene.gltf"), so it asks rather than silently
// reusing the old one.
class ImportConflictModal final : public EditorModal
{
  public:
    struct Conflict
    {
        std::string sourcePath;
        std::string destinationDirectory;
        std::string existingFolderName;
        std::string keepBothFolderName;
        Kn5ImportOptions kn5Options;
    };

    ImportConflictModal();

    void Ask(Conflict conflict);
    // Waiting for the user to choose.
    bool IsPending() const
    {
        return m_pending.has_value();
    }
    // Dismissed without an explicit choice (e.g. Escape): treated as cancel.
    void OnClose(EditorContext& context) override;

  protected:
    void OnGui(EditorContext& context) override;

  private:
    std::optional<Conflict> m_pending;
};
}
