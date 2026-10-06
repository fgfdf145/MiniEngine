#pragma once

#include <engine/asset/kn5_importer.h>
#include <engine/core/threading/task_future.h>
#include <engine/editor/ui/framework/editor_modal.h>

#include <optional>
#include <string>
#include <vector>

namespace me
{

// Asks for an Assetto Corsa model's livery, track layout and conversion options before importing it.
// The model is surveyed on a background thread: a track's kn5 runs to hundreds of megabytes.
class Kn5ImportModal final : public EditorModal
{
  public:
    Kn5ImportModal();

    // Surveys `sourcePath` (a .kn5 or a track's models.ini) and asks about it.
    void Ask(const std::string& sourcePath, const std::string& destinationDirectory);
    // Waiting for the user to choose.
    bool IsPending() const
    {
        return m_pending.has_value();
    }

    // Lets go of the surveys of dialogs cancelled before they finished, once they have.
    void Tick(EditorContext& context) override;
    // Dismissed without an explicit choice (e.g. Escape): treated as cancel.
    void OnClose(EditorContext& context) override;

  protected:
    void OnGui(EditorContext& context) override;

  private:
    struct PendingImport
    {
        std::string sourcePath;
        std::string destinationDirectory;
        TaskFuture<Kn5ModelSummary> survey;
        std::optional<Kn5ModelSummary> summary;
        std::string error;
        size_t selectedSkin = 0;
        // 0: the picked file; n: summary->layouts[n - 1].
        size_t selectedLayout = 0;
        Kn5ImportOptions options;
    };

    void Cancel();

    std::optional<PendingImport> m_pending;
    // Surveys of dialogs cancelled before they finished, kept until done so cancelling never
    // waits on one.
    std::vector<TaskFuture<Kn5ModelSummary>> m_abandonedSurveys;
};
}
