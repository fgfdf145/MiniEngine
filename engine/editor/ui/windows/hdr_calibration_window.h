#pragma once

#include <engine/editor/ui/framework/editor_window.h>
#include <engine/renderer/display_calibration.h>

namespace me
{

// Graphics Debug > HDR Calibration and Render > HDR Calibration: the PS5's Adjust HDR (maximum
// full-frame and 10 % window luminance, black level), then the scene's paper white over the live
// scene (docs/design/2026-10-10-hdr-calibration-design.md). The viewport goes fullscreen and shows
// each step's pattern; this window sits over it at the bottom.
class HdrCalibrationWindow final : public EditorWindow
{
  public:
    enum class Step
    {
        Start,
        FullFrame,
        Window,
        Black,
        Review
    };

    HdrCalibrationWindow();

    bool DrawsInFullscreen() const override
    {
        return true;
    }
    void Tick(EditorContext& context) override;
    void OnOpen(EditorContext& context) override;
    void OnClose(EditorContext& context) override;

    static Step NextStep(Step step);
    static Step PreviousStep(Step step);

  protected:
    void OnGui(EditorContext& context) override;
    void PreBegin(EditorContext& context) override;
    ImGuiWindowFlags GetWindowFlags(const EditorContext& context) const override;

  private:
    // Saves what the calibration changes and starts it; once per opening.
    void Begin(EditorContext& context);
    void EnterStep(EditorContext& context, Step step);
    void DrawStart(EditorContext& context);
    // A trial level's slider with the arrow keys stepping it.
    void DrawLevel(EditorContext& context);
    void DrawReview(EditorContext& context);
    void DrawButtons(EditorContext& context);
    // What the pattern shows for the current step and level.
    void UpdateView(EditorContext& context);
    void Finish(EditorContext& context);
    void Cancel(EditorContext& context);

    Step m_step = Step::Start;
    // The settings being worked on, live in the renderer while the window is open, and the ones to
    // go back to on Cancel.
    DisplaySettings m_work;
    DisplaySettings m_saved;
    bool m_finished = false;
    // The editor's state the calibration changed, put back when it closes.
    bool m_wasFullscreen = false;
    bool m_hadViewportUi = true;
    bool m_hadGizmos = true;
    bool m_focusPending = false;
    // Between Begin and OnClose.
    bool m_active = false;
};
}
