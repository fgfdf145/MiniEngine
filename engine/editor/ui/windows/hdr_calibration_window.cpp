#include "hdr_calibration_window.h"

#include <engine/editor/ui/framework/editor_context.h>
#include <engine/editor/ui_colors.h>

#include <imgui.h>

#include <algorithm>
#include <utility>

namespace me
{

namespace
{
// How a step's trial level moves: between lo and hi (in PQ, so each notch looks the same size) in
// notches steps, one per arrow key press.
struct LevelRange
{
    float lo = 0.0f;
    float hi = 1.0f;
    int notches = 64;
};

LevelRange RangeOf(HdrCalibrationWindow::Step step)
{
    using Step = HdrCalibrationWindow::Step;
    switch (step)
    {
    case Step::FullFrame:
    case Step::Window:
        return {PqFromNits(kMinCalibrationPeakNits), PqFromNits(kMaxCalibrationPeakNits), 96};
    case Step::Black:
        return {0.0f, PqFromNits(kMaxCalibrationBlackNits), 64};
    default:
        return {};
    }
}

float* LevelOf(HdrCalibrationWindow::Step step, DisplaySettings& settings)
{
    using Step = HdrCalibrationWindow::Step;
    switch (step)
    {
    case Step::FullFrame:
        return &settings.maxFullFrameLuminance;
    case Step::Window:
        return &settings.maxLuminance;
    case Step::Black:
        return &settings.minLuminance;
    default:
        return nullptr;
    }
}

CalibrationPattern PatternOf(HdrCalibrationWindow::Step step)
{
    using Step = HdrCalibrationWindow::Step;
    switch (step)
    {
    case Step::FullFrame:
        return CalibrationPattern::HdrFullFrame;
    case Step::Window:
        return CalibrationPattern::HdrWindow;
    case Step::Black:
        return CalibrationPattern::HdrBlack;
    default:
        return CalibrationPattern::None;
    }
}

const char* TitleOf(HdrCalibrationWindow::Step step)
{
    using Step = HdrCalibrationWindow::Step;
    switch (step)
    {
    case Step::Start:
        return "HDR Calibration";
    case Step::FullFrame:
        return "1 / 4  Maximum luminance (full screen)";
    case Step::Window:
        return "2 / 4  Maximum luminance (10 % window)";
    case Step::Black:
        return "3 / 4  Minimum luminance (black)";
    case Step::Review:
        return "4 / 4  Paper white and review";
    }
    return "";
}

const char* InstructionOf(HdrCalibrationWindow::Step step)
{
    using Step = HdrCalibrationWindow::Step;
    switch (step)
    {
    case Step::FullFrame:
    case Step::Window:
        return "Raise the level until the ring in the middle just disappears. Step back down if it is "
               "gone already: the level where it first vanishes is where the display clips.";
    case Step::Black:
        return "Lower the level until the ring just disappears into the background. Darker than this, "
               "the display shows no difference from black.";
    case Step::Review:
        return "The scene with the calibration. Paper white sets how bright the scene's midtones are; "
               "following the UI white keeps them as bright as in SDR.";
    default:
        return "";
    }
}
}

HdrCalibrationWindow::HdrCalibrationWindow()
    : EditorWindow("hdr_calibration", "HDR Calibration")
{
}

HdrCalibrationWindow::Step HdrCalibrationWindow::NextStep(Step step)
{
    switch (step)
    {
    case Step::Start:
        return Step::FullFrame;
    case Step::FullFrame:
        return Step::Window;
    case Step::Window:
        return Step::Black;
    case Step::Black:
    case Step::Review:
        return Step::Review;
    }
    return Step::Review;
}

HdrCalibrationWindow::Step HdrCalibrationWindow::PreviousStep(Step step)
{
    switch (step)
    {
    case Step::Start:
    case Step::FullFrame:
        return Step::Start;
    case Step::Window:
        return Step::FullFrame;
    case Step::Black:
        return Step::Window;
    case Step::Review:
        return Step::Black;
    }
    return Step::Start;
}

void HdrCalibrationWindow::OnOpen(EditorContext& context)
{
    Begin(context);
}

void HdrCalibrationWindow::Begin(EditorContext& context)
{
    if (m_active)
    {
        return;
    }
    m_active = true;
    EditorSharedState& state = context.state;
    m_saved = state.renderDebug.display;
    m_work = m_saved;
    m_finished = false;
    m_step = Step::Start;
    // The patterns fill the screen, with nothing of the editor over them but this window.
    m_wasFullscreen = state.commands.viewportFullscreen;
    m_hadViewportUi = state.commands.viewportUi;
    m_hadGizmos = state.commands.gizmos;
    state.commands.viewportFullscreen = true;
    state.commands.viewportUi = false;
    state.commands.gizmos = false;
    EnterStep(context, Step::Start);
    m_focusPending = true;
}

void HdrCalibrationWindow::OnClose(EditorContext& context)
{
    if (!std::exchange(m_active, false))
    {
        return;
    }
    EditorSharedState& state = context.state;
    if (!m_finished)
    {
        state.renderDebug.display = m_saved;
    }
    state.renderDebug.calibrationView = {};
    state.commands.viewportFullscreen = m_wasFullscreen;
    state.commands.viewportUi = m_hadViewportUi;
    state.commands.gizmos = m_hadGizmos;
}

void HdrCalibrationWindow::Tick(EditorContext& context)
{
    if (!IsOpen())
    {
        return;
    }
    // The manager calls OnOpen only after the first Draw; this frame's Tick comes before both.
    Begin(context);
    // Escape or F11 left the fullscreen viewport: that is Cancel.
    if (!context.state.commands.viewportFullscreen)
    {
        m_wasFullscreen = false;
        Cancel(context);
        return;
    }
    context.state.renderDebug.display = m_work;
    UpdateView(context);
}

void HdrCalibrationWindow::EnterStep(EditorContext& context, Step step)
{
    // The luminances start from what is in use: the last calibration or the display's own figures.
    if (m_step == Step::Start && step != Step::Start && !m_work.calibrated)
    {
        const DisplayOutput& output = context.state.display.output;
        m_work.maxLuminance = output.maxLuminance;
        m_work.maxFullFrameLuminance = output.maxFullFrameLuminance;
        m_work.minLuminance = output.minLuminance;
    }
    if (step == Step::FullFrame || step == Step::Window || step == Step::Black)
    {
        m_work.calibrated = true;
    }
    m_step = step;
    UpdateView(context);
}

void HdrCalibrationWindow::UpdateView(EditorContext& context)
{
    DisplayCalibrationView& view = context.state.renderDebug.calibrationView;
    view.pattern = PatternOf(m_step);
    const float* level = LevelOf(m_step, m_work);
    view.level = level != nullptr ? *level : 0.0f;
}

void HdrCalibrationWindow::PreBegin(EditorContext& context)
{
    static_cast<void>(context);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y - 40.0f * UiScale()),
        ImGuiCond_Always,
        ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(620.0f * UiScale(), 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88f);
    // Focused, so the arrow keys and Enter reach it at once.
    if (std::exchange(m_focusPending, false))
    {
        ImGui::SetNextWindowFocus();
    }
}

ImGuiWindowFlags HdrCalibrationWindow::GetWindowFlags(const EditorContext& context) const
{
    static_cast<void>(context);
    // The arrow keys move the level, not ImGui's keyboard focus.
    return ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;
}

void HdrCalibrationWindow::OnGui(EditorContext& context)
{
    ImGui::SeparatorText(TitleOf(m_step));
    const char* instruction = InstructionOf(m_step);
    if (*instruction != '\0')
    {
        ImGui::TextWrapped("%s", instruction);
        ImGui::Spacing();
    }

    switch (m_step)
    {
    case Step::Start:
        DrawStart(context);
        break;
    case Step::Review:
        DrawReview(context);
        break;
    default:
        DrawLevel(context);
        break;
    }
    ImGui::Spacing();
    DrawButtons(context);
}

void HdrCalibrationWindow::DrawStart(EditorContext& context)
{
    const EditorDisplayStatus& display = context.state.display;
    const platform::display::DisplayHdrInfo& report = display.report;
    if (report.known)
    {
        ImGui::Text("Windows: HDR %s on %s", report.hdrEnabled ? "on" : "off", report.name.c_str());
        ImGui::Text(
            "The display reports %.0f cd/m^2 peak, %.0f full screen, %.4f black.",
            report.maxLuminance,
            report.maxFullFrameLuminance,
            report.minLuminance);
        ImGui::Text("SDR content brightness (Windows): %.0f cd/m^2, the editor's UI white.", report.sdrWhiteNits);
    }
    else
    {
        ImGui::TextUnformatted("The OS reports nothing about this display.");
    }
    if (!display.output.hdr)
    {
        ImGui::TextColored(ui_colors::kTextWarning, "The output is SDR: turn on HDR output and Windows' Use HDR first.");
    }
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Next: the three luminance screens of the PS5's Adjust HDR, then the scene's paper white. "
        "Sit facing the middle of the screen, in the light you play in. Escape cancels.");
}

void HdrCalibrationWindow::DrawLevel(EditorContext& context)
{
    float* level = LevelOf(m_step, m_work);
    if (level == nullptr)
    {
        return;
    }
    const LevelRange range = RangeOf(m_step);
    const float notch = (range.hi - range.lo) / static_cast<float>(range.notches);
    float position = PqFromNits(*level);

    bool changed = false;
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
    {
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow))
        {
            position += notch;
            changed = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow))
        {
            position -= notch;
            changed = true;
        }
    }
    if (ImGui::ArrowButton("##down", ImGuiDir_Left))
    {
        position -= notch;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(380.0f * UiScale());
    changed = ImGui::SliderFloat("##level", &position, range.lo, range.hi, "") || changed;
    ImGui::SameLine();
    if (ImGui::ArrowButton("##up", ImGuiDir_Right))
    {
        position += notch;
        changed = true;
    }
    position = std::clamp(position, range.lo, range.hi);
    if (changed)
    {
        *level = NitsFromPq(position);
        // The full-frame peak cannot be above the small window's.
        if (m_step == Step::Window)
        {
            m_work.maxFullFrameLuminance = std::min(m_work.maxFullFrameLuminance, m_work.maxLuminance);
        }
        UpdateView(context);
    }

    ImGui::SameLine();
    ImGui::Text(m_step == Step::Black ? "%.3f cd/m^2" : "%.0f cd/m^2", *level);
    ImGui::TextDisabled("Left / Right arrows step the level, Enter goes on, Backspace goes back.");
}

void HdrCalibrationWindow::DrawReview(EditorContext& context)
{
    const DisplayOutput& output = context.state.display.output;
    // The scene's paper white: by default where Windows shows SDR white, so the midtones are as bright
    // as in SDR and only the highlights go further; 250 is GT7's absolute scale.
    bool follow = m_work.paperWhiteNits <= 0.0f;
    if (ImGui::Checkbox("Paper white follows the UI white", &follow))
    {
        m_work.paperWhiteNits = follow ? 0.0f : output.paperWhiteNits;
    }
    ImGui::BeginDisabled(follow);
    float paperWhite = follow ? output.paperWhiteNits : m_work.paperWhiteNits;
    ImGui::SetNextItemWidth(380.0f * UiScale());
    if (ImGui::SliderFloat("Paper white", &paperWhite, kMinUiWhiteNits, kMaxUiWhiteNits, "%.0f cd/m^2") && !follow)
    {
        m_work.paperWhiteNits = paperWhite;
    }
    ImGui::EndDisabled();

    // Fixed-fit columns: the window sizes itself to its contents, so stretched ones would have no width.
    if (ImGui::BeginTable("##review", 2, ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed, 300.0f * UiScale());
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthFixed, 160.0f * UiScale());
        const auto row = [](const char* name, const char* format, float value)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name);
            ImGui::TableNextColumn();
            ImGui::Text(format, value);
        };
        row("Maximum luminance (full screen)", "%.0f cd/m^2", m_work.maxFullFrameLuminance);
        row("Maximum luminance (10 % window)", "%.0f cd/m^2", m_work.maxLuminance);
        row("Minimum luminance", "%.3f cd/m^2", m_work.minLuminance);
        row("UI white", "%.0f cd/m^2", output.uiWhiteNits);
        row("Paper white", "%.0f cd/m^2", output.paperWhiteNits);
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Finish keeps these (saved with the engine settings); Cancel puts back the old ones.");
}

void HdrCalibrationWindow::DrawButtons(EditorContext& context)
{
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (m_step != Step::Start)
    {
        if (ImGui::Button("Back") || (focused && ImGui::IsKeyPressed(ImGuiKey_Backspace, false)))
        {
            EnterStep(context, PreviousStep(m_step));
            return;
        }
        ImGui::SameLine();
    }
    const char* next = m_step == Step::Start ? "Start" : m_step == Step::Review ? "Finish" : "Next";
    // The patterns mean nothing on an SDR swapchain.
    ImGui::BeginDisabled(m_step == Step::Start && !context.state.display.output.hdr);
    const bool nextPressed =
        ImGui::Button(next) || (focused && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)));
    ImGui::EndDisabled();
    if (nextPressed && (m_step != Step::Start || context.state.display.output.hdr))
    {
        if (m_step == Step::Review)
        {
            Finish(context);
        }
        else
        {
            EnterStep(context, NextStep(m_step));
        }
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        Cancel(context);
    }
}

void HdrCalibrationWindow::Finish(EditorContext& context)
{
    m_finished = true;
    context.state.renderDebug.display = m_work;
    Close();
}

void HdrCalibrationWindow::Cancel(EditorContext& context)
{
    m_finished = false;
    context.state.renderDebug.display = m_saved;
    context.state.renderDebug.calibrationView = {};
    Close();
}
}
