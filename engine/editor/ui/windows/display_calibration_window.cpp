#include "display_calibration_window.h"

#include <engine/editor/ui/framework/editor_context.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace me
{

namespace
{
// How a step's trial level moves: between lo and hi in notches steps (the arrow keys move one), in
// PQ for luminances so each notch looks the same size, or in sRGB signal for the SDR corrections.
struct LevelRange
{
    float lo = 0.0f;
    float hi = 1.0f;
    int notches = 64;
    bool pq = false;
};

LevelRange RangeOf(DisplayCalibrationWindow::Step step)
{
    using Step = DisplayCalibrationWindow::Step;
    switch (step)
    {
    case Step::HdrFullFrame:
    case Step::HdrWindow:
        return {PqFromNits(kMinCalibrationPeakNits), PqFromNits(kMaxCalibrationPeakNits), 96, true};
    case Step::HdrBlack:
        return {0.0f, PqFromNits(kMaxCalibrationBlackNits), 64, true};
    case Step::SdrBright:
        return {0.80f, 1.0f, 40, false};
    case Step::SdrDark:
        return {0.0f, 0.15f, 30, false};
    default:
        return {};
    }
}

float* LevelOf(DisplayCalibrationWindow::Step step, DisplaySettings& settings)
{
    using Step = DisplayCalibrationWindow::Step;
    switch (step)
    {
    case Step::HdrFullFrame:
        return &settings.maxFullFrameLuminance;
    case Step::HdrWindow:
        return &settings.maxLuminance;
    case Step::HdrBlack:
        return &settings.minLuminance;
    case Step::SdrBright:
        return &settings.sdrWhite;
    case Step::SdrDark:
        return &settings.sdrBlack;
    default:
        return nullptr;
    }
}

CalibrationPattern PatternOf(DisplayCalibrationWindow::Step step, int picture)
{
    using Step = DisplayCalibrationWindow::Step;
    switch (step)
    {
    case Step::HdrFullFrame:
        return CalibrationPattern::HdrFullFrame;
    case Step::HdrWindow:
        return CalibrationPattern::HdrWindow;
    case Step::HdrBlack:
        return CalibrationPattern::HdrBlack;
    case Step::SdrBright:
        return CalibrationPattern::SdrBright;
    case Step::SdrDark:
        return CalibrationPattern::SdrDark;
    case Step::Picture:
        return picture == 1 ? CalibrationPattern::SampleWedge : picture == 2 ? CalibrationPattern::SampleSky : CalibrationPattern::None;
    default:
        return CalibrationPattern::None;
    }
}

constexpr std::array<const char*, 3> kPictureNames = {"Scene", "Step wedge", "Sky and sun"};
constexpr std::array<const char*, 3> kOutputModeNames = {"Auto (follow Windows)", "HDR", "SDR"};

const char* TitleOf(DisplayCalibrationWindow::Step step, bool hdr)
{
    using Step = DisplayCalibrationWindow::Step;
    switch (step)
    {
    case Step::Start:
        return "Display Calibration";
    case Step::HdrFullFrame:
        return "1 / 5  Maximum luminance (full screen)";
    case Step::HdrWindow:
        return "2 / 5  Maximum luminance (10 % window)";
    case Step::HdrBlack:
        return "3 / 5  Minimum luminance (black)";
    case Step::SdrBright:
        return "1 / 4  Bright section correction";
    case Step::SdrDark:
        return "2 / 4  Dark section correction";
    case Step::Picture:
        return hdr ? "4 / 5  Exposure and saturation" : "3 / 4  Exposure and saturation";
    case Step::Review:
        return hdr ? "5 / 5  Review" : "4 / 4  Review";
    }
    return "";
}

const char* InstructionOf(DisplayCalibrationWindow::Step step)
{
    using Step = DisplayCalibrationWindow::Step;
    switch (step)
    {
    case Step::HdrFullFrame:
    case Step::HdrWindow:
        return "Raise the level until the ring in the middle just disappears. Step back down if it is "
               "gone already: the level where it first vanishes is where the display clips.";
    case Step::HdrBlack:
        return "Lower the level until the ring just disappears into the background. Darker than this, "
               "the display shows no difference from black.";
    case Step::SdrBright:
        return "If the checkerboard in the middle cannot be seen, lower the level until it just appears.";
    case Step::SdrDark:
        return "If the checkerboard in the middle cannot be seen, raise the level until it just appears.";
    case Step::Picture:
        return "Adjust the exposure and saturation to taste. Q and E switch the picture.";
    default:
        return "";
    }
}
}

DisplayCalibrationWindow::DisplayCalibrationWindow()
    : EditorWindow("display_calibration", "Display Calibration")
{
}

DisplayCalibrationWindow::Step DisplayCalibrationWindow::NextStep(Step step, bool hdr)
{
    switch (step)
    {
    case Step::Start:
        return hdr ? Step::HdrFullFrame : Step::SdrBright;
    case Step::HdrFullFrame:
        return Step::HdrWindow;
    case Step::HdrWindow:
        return Step::HdrBlack;
    case Step::SdrBright:
        return Step::SdrDark;
    case Step::HdrBlack:
    case Step::SdrDark:
        return Step::Picture;
    case Step::Picture:
    case Step::Review:
        return Step::Review;
    }
    return Step::Review;
}

DisplayCalibrationWindow::Step DisplayCalibrationWindow::PreviousStep(Step step, bool hdr)
{
    switch (step)
    {
    case Step::Start:
    case Step::HdrFullFrame:
    case Step::SdrBright:
        return Step::Start;
    case Step::HdrWindow:
        return Step::HdrFullFrame;
    case Step::HdrBlack:
        return Step::HdrWindow;
    case Step::SdrDark:
        return Step::SdrBright;
    case Step::Picture:
        return hdr ? Step::HdrBlack : Step::SdrDark;
    case Step::Review:
        return Step::Picture;
    }
    return Step::Start;
}

void DisplayCalibrationWindow::OnOpen(EditorContext& context)
{
    Begin(context);
}

void DisplayCalibrationWindow::Begin(EditorContext& context)
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
    m_picture = 0;
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

void DisplayCalibrationWindow::OnClose(EditorContext& context)
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

void DisplayCalibrationWindow::Tick(EditorContext& context)
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

void DisplayCalibrationWindow::EnterStep(EditorContext& context, Step step)
{
    // The luminances start from what is in use: the last calibration or the display's own figures.
    if (m_step == Step::Start && step != Step::Start && !m_work.calibrated)
    {
        const DisplayOutput& output = context.state.display.output;
        m_work.maxLuminance = output.maxLuminance;
        m_work.maxFullFrameLuminance = output.maxFullFrameLuminance;
        m_work.minLuminance = output.minLuminance;
    }
    if (step == Step::HdrFullFrame || step == Step::HdrWindow || step == Step::HdrBlack)
    {
        m_work.calibrated = true;
    }
    m_step = step;
    UpdateView(context);
}

void DisplayCalibrationWindow::UpdateView(EditorContext& context)
{
    DisplayCalibrationView& view = context.state.renderDebug.calibrationView;
    view.pattern = PatternOf(m_step, m_picture);
    const float* level = LevelOf(m_step, m_work);
    view.level = level != nullptr ? *level : 0.0f;
}

void DisplayCalibrationWindow::PreBegin(EditorContext& context)
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

ImGuiWindowFlags DisplayCalibrationWindow::GetWindowFlags(const EditorContext& context) const
{
    static_cast<void>(context);
    // The arrow keys move the level, not ImGui's keyboard focus.
    return ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;
}

void DisplayCalibrationWindow::OnGui(EditorContext& context)
{
    const bool hdr = context.state.display.hdrActive;
    ImGui::SeparatorText(TitleOf(m_step, hdr));
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
    case Step::Picture:
        DrawPicture(context);
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

void DisplayCalibrationWindow::DrawStart(EditorContext& context)
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
        if (report.hdrEnabled)
        {
            ImGui::Text("SDR content brightness (Windows): %.0f cd/m^2 - the editor's UI white.", report.sdrWhiteNits);
        }
    }
    else
    {
        ImGui::TextUnformatted("The OS reports nothing about this display.");
    }

    int mode = static_cast<int>(m_work.outputMode);
    ImGui::SetNextItemWidth(220.0f * UiScale());
    if (ImGui::Combo("Output", &mode, kOutputModeNames.data(), static_cast<int>(kOutputModeNames.size())))
    {
        m_work.outputMode = static_cast<DisplayOutputMode>(mode);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("now %s", display.hdrActive ? "HDR10" : "SDR");
    if (display.hdrRequested && !display.hdrActive)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "HDR was asked for, but the display offers no HDR10 format.");
    }
    else if (!display.hdrActive && report.hdrEnabled)
    {
        ImGui::TextDisabled("Windows is in HDR: Auto or HDR shows the scene in HDR.");
    }

    ImGui::Spacing();
    ImGui::TextWrapped(
        "%s",
        display.hdrActive
            ? "Next: the three luminance screens of the PS5's Adjust HDR, then GT7's exposure and saturation. "
              "Sit facing the middle of the screen, in the light you play in."
            : "Next: GT7's bright and dark section correction for SDR, then exposure and saturation. "
              "Sit facing the middle of the screen, in the light you play in.");
    if (m_work.calibrated)
    {
        if (ImGui::Button("Use the display's own figures"))
        {
            m_work.calibrated = false;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("forgets the calibrated luminances");
    }
}

bool DisplayCalibrationWindow::DrawLevel(EditorContext& context)
{
    float* level = LevelOf(m_step, m_work);
    if (level == nullptr)
    {
        return false;
    }
    const LevelRange range = RangeOf(m_step);
    const float notch = (range.hi - range.lo) / static_cast<float>(range.notches);
    float position = range.pq ? PqFromNits(*level) : *level;

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
        *level = range.pq ? NitsFromPq(position) : position;
        // The full-frame peak cannot be above the small window's.
        if (m_step == Step::HdrWindow)
        {
            m_work.maxFullFrameLuminance = std::min(m_work.maxFullFrameLuminance, m_work.maxLuminance);
        }
        UpdateView(context);
    }

    ImGui::SameLine();
    if (range.pq)
    {
        ImGui::Text(m_step == Step::HdrBlack ? "%.3f cd/m^2" : "%.0f cd/m^2", *level);
    }
    else
    {
        ImGui::Text("%.0f / 255", *level * 255.0f);
    }
    ImGui::TextDisabled("Left / Right arrows step the level, Enter goes on.");
    return changed;
}

void DisplayCalibrationWindow::DrawPicture(EditorContext& context)
{
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
    {
        const int count = static_cast<int>(kPictureNames.size());
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false))
        {
            m_picture = (m_picture + count - 1) % count;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_E, false))
        {
            m_picture = (m_picture + 1) % count;
        }
    }
    for (int index = 0; index < static_cast<int>(kPictureNames.size()); ++index)
    {
        if (index > 0)
        {
            ImGui::SameLine();
        }
        if (ImGui::RadioButton(kPictureNames[index], m_picture == index))
        {
            m_picture = index;
        }
    }
    ImGui::SetNextItemWidth(380.0f * UiScale());
    ImGui::SliderFloat("Exposure", &m_work.exposureEv, -2.0f, 2.0f, "%+.1f EV");
    ImGui::SetNextItemWidth(380.0f * UiScale());
    ImGui::SliderFloat("Saturation", &m_work.saturation, 0.0f, 2.0f, "%.2f");
    if (ImGui::Button("Reset exposure and saturation"))
    {
        m_work.exposureEv = 0.0f;
        m_work.saturation = 1.0f;
    }
    UpdateView(context);
}

void DisplayCalibrationWindow::DrawReview(EditorContext& context)
{
    const bool hdr = context.state.display.hdrActive;
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
        if (hdr)
        {
            row("Maximum luminance (full screen)", "%.0f cd/m^2", m_work.maxFullFrameLuminance);
            row("Maximum luminance (10 % window)", "%.0f cd/m^2", m_work.maxLuminance);
            row("Minimum luminance", "%.3f cd/m^2", m_work.minLuminance);
            row("UI white (SDR content brightness)", "%.0f cd/m^2", context.state.display.output.uiWhiteNits);
        }
        else
        {
            row("Bright section (white signal)", "%.0f / 255", m_work.sdrWhite * 255.0f);
            row("Dark section (black signal)", "%.0f / 255", m_work.sdrBlack * 255.0f);
        }
        row("Exposure", "%+.1f EV", m_work.exposureEv);
        row("Saturation", "%.2f", m_work.saturation);
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Finish keeps these (saved with the engine settings); Cancel puts back the old ones.");
}

void DisplayCalibrationWindow::DrawButtons(EditorContext& context)
{
    const bool hdr = context.state.display.hdrActive;
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (m_step != Step::Start)
    {
        if (ImGui::Button("Back") || (focused && ImGui::IsKeyPressed(ImGuiKey_Backspace, false)))
        {
            EnterStep(context, PreviousStep(m_step, hdr));
            return;
        }
        ImGui::SameLine();
    }
    const char* next = m_step == Step::Start ? "Start" : m_step == Step::Review ? "Finish" : "Next";
    if (ImGui::Button(next) || (focused && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))))
    {
        if (m_step == Step::Review)
        {
            Finish(context);
        }
        else
        {
            EnterStep(context, NextStep(m_step, hdr));
        }
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        Cancel(context);
    }
}

void DisplayCalibrationWindow::Finish(EditorContext& context)
{
    m_finished = true;
    context.state.renderDebug.display = m_work;
    Close();
}

void DisplayCalibrationWindow::Cancel(EditorContext& context)
{
    m_finished = false;
    context.state.renderDebug.display = m_saved;
    context.state.renderDebug.calibrationView = {};
    Close();
}
}
